// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file dataCreatorDV0Reduced.cxx
/// \brief Creation of D-V0 pairs
///
/// \author Luca Aglietta <luca.aglietta@cern.ch>, UniTO Turin

#include <cmath>
#include <map>

#include "DetectorsBase/Propagator.h"
#include "CommonConstants/PhysicsConstants.h"
#include "DCAFitter/DCAFitterN.h"
#include "Framework/AnalysisTask.h"
#include "Framework/O2DatabasePDGPlugin.h"
#include "Framework/runDataProcessing.h"
#include "ReconstructionDataFormats/DCA.h"

#include "Common/Core/trackUtilities.h"
#include "Common/DataModel/CollisionAssociationTables.h"

#include "PWGLF/DataModel/LFStrangenessTables.h"

#include "PWGHF/Core/HfHelper.h"
#include "PWGHF/DataModel/CandidateReconstructionTables.h"
#include "PWGHF/DataModel/CandidateSelectionTables.h"
#include "PWGHF/Utils/utilsBfieldCCDB.h"
#include "PWGHF/D2H/DataModel/ReducedDataModel.h"

using namespace o2;
using namespace o2::analysis;
using namespace o2::aod;
using namespace o2::constants::physics;
using namespace o2::framework;
using namespace o2::framework::expressions;

// event types
enum Event : uint8_t {
  Processed = 0,
  NoDV0Selected,
  DV0Selected,
  kNEvent
};

enum DecayChannel : uint8_t {
  DstarV0 = 0,
  DplusV0
  };

enum V0_type : uint8_t {
  K0s = 0,
  Lambda,
  AntiLambda
};

enum TypeD : uint8_t {
  Dplus = 1,
  Dstar
};

/// Creation of D-V0 pairs
struct HfDataCreatorDV0Reduced {
  // Produces AOD tables to store track information
  Produces<aod::HfRedCollision> hfReducedCollision;  // Defined in PWGLF/DataModel/LFStrangenessTables.h
  Produces<aod::HfOrigColCounts> hfCollisionCounter; // Defined in PWGHF/D2H/DataModel/ReducedDataModel.h

  Produces<aod::HfRedVzeros> hfCandV0;   // Defined in PWGHF/D2H/DataModel/ReducedDataModel.h
  Produces<aod::HfRed3PrNoTrks> hfCandD; // Defined in PWGHF/D2H/DataModel/ReducedDataModel.h

  // CCDB configuration
  o2::ccdb::CcdbApi ccdbApi;
  Service<o2::ccdb::BasicCCDBManager> ccdb;
  Configurable<std::string> url{"ccdb-url", "http://alice-ccdb.cern.ch", "url of the ccdb repository"};
  Configurable<std::string> ccdbPathGrpMag{"ccdbPathGrpMag", "GLO/Config/GRPMagField", "CCDB path of the GRPMagField object (Run 3)"};
  Configurable<bool> propagateV0toPV{"propagateV0toPV", false, "Enable or disable V0 propagation to V0"};

  int runNumber{0}; // needed to detect if the run changed and trigger update of calibrations etc.
  // selection D
  Configurable<int> selectionFlagDplus{"selectionFlagDplus", 7, "Selection Flag for D"};
  Configurable<bool> selectionFlagDstarToD0Pi{"selectionFlagDstarToD0Pi", true, "Selection Flag for D* decay to D0 & Pi"};
  // selection V0
  Configurable<float> minK0sLambdaCosinePa{"minK0sLambdaCosinePa", 0.97, "minimum cosp for K0S and Lambda"};
  Configurable<float> minK0sLambdaRadius{"minK0sLambdaRadius", 0.5, "minimum radius for K0S and Lambda"};
  Configurable<float> deltaMassK0s{"deltaMassK0s", 0.03, "delta mass cut for K0S"};
  Configurable<float> deltaMassLambda{"deltaMassLambda", 0.015, "delta mass cut for Lambda"};
  Configurable<float> minV0dauEta{"minV0dauEta", 1., "minimum eta for V0 daughters"};
  Configurable<float> maxV0DCA{"maxV0DCA", 0.1, "maximum DCA for K0S and Lambda"};
  Configurable<float> minV0dauDCA{"minV0dauDCA", 0.05, "minimum DCA for V0 daughters"};
  Configurable<float> maxV0dauDCA{"maxV0dauDCA", 1., "maximum DCA for V0 daughters"};

  // material correction for track propagation
  o2::base::MatLayerCylSet* lut;
  o2::base::Propagator::MatCorrType matCorr = o2::base::Propagator::MatCorrType::USEMatCorrLUT;
  HfHelper hfHelper;

  bool isHfCandResoConfigFilled = false;

  using CandsDplusFiltered = soa::Filtered<soa::Join<aod::HfCand3Prong, aod::HfSelDplusToPiKPi>>;
  using CandDstarFiltered = soa::Filtered<soa::Join<aod::HfD0FromDstar, aod::HfCandDstar, aod::HfSelDstarToD0Pi>>;

  Filter filterSelectDplus = (aod::hf_sel_candidate_dplus::isSelDplusToPiKPi >= selectionFlagDplus);
  Filter filterSelectedCandDstar = (aod::hf_sel_candidate_dstar::isSelDstarToD0Pi == selectionFlagDstarToD0Pi);

  Preslice<CandsDplusFiltered> candsDplusPerCollision = aod::track_association::collisionId;
  Preslice<CandDstarFiltered> candsDstarPerCollision = aod::track_association::collisionId;
  Preslice<aod::TrackAssoc> trackIndicesPerCollision = aod::track_association::collisionId;
  Preslice<aod::V0Datas> candsV0PerCollision = aod::track_association::collisionId;

  HistogramRegistry registry{"registry"};

  void init(InitContext const&)
  {
    // histograms
    constexpr int kNBinsEvents = kNEvent;
    std::string labels[kNBinsEvents];
    labels[Event::Processed] = "processed";
    labels[Event::NoDV0Selected] = "without DV0 pairs";
    labels[Event::DV0Selected] = "with DV0 pairs";
    static const AxisSpec axisEvents = {kNBinsEvents, 0.5, kNBinsEvents + 0.5, ""};
    registry.add("hEvents", "Events;;entries", HistType::kTH1F, {axisEvents});
    for (int iBin = 0; iBin < kNBinsEvents; iBin++) {
      registry.get<TH1>(HIST("hEvents"))->GetXaxis()->SetBinLabel(iBin + 1, labels[iBin].data());
    }
    registry.add("hMassDplus", "Dplus candidates;inv. mass (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 1.7, 2}}});
    registry.add("hMassDstar", "Dstar candidates;inv. mass (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 0.05, 0.25}}});
    registry.add("hMassK0s", "K0^{s} candidates;inv. mass (#pi^{#plus}#pi^{#minus}) (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 0.35, 0.65}}});
    registry.add("hMassLambda", "Lambda candidates;inv. mass (p #pi^{#minus}) (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 1.05, 1.35}}});
    registry.add("hPtDplus", "D^{#minus} candidates;D^{#minus} candidate #it{p}_{T} (GeV/#it{c});entries", {HistType::kTH1F, {{100, 0., 10.}}});
    registry.add("hPtDstar", "D^{*} candidates;D^{*} candidate #it{p}_{T} (GeV/#it{c});entries", {HistType::kTH1F, {{100, 0., 10.}}});
    registry.add("hPtV0", "V0 candidates;V0 candidate #it{p}_{T} (GeV/#it{c});entries", {HistType::kTH1F, {{100, 0., 10.}}});
    registry.add("hMassDs1", "Ds1 candidates;m_{Ds1} - m_{D^{*}} (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 0.45, 0.7}}});
    registry.add("hMassDsStar2", "Ds^{*}2 candidates; Ds^{*}2 - m_{D^{#plus}} (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 0.4, 1}}});
    registry.add("hMassXcRes", "XcRes candidates; XcRes - m_{D^{#plus}} (GeV/#it{c}^{2});entries", {HistType::kTH1F, {{100, 1., 1.4}}});
    registry.add("hV0_type", "V0 selection flag", {HistType::kTH1F, {{8, -0.5, 7.5}}});
    registry.add("hD_type", "D selection flag", {HistType::kTH1F, {{5, -2.5, 2.5}}});

    ccdb->setURL(url.value);
    ccdb->setCaching(true);
    ccdb->setLocalObjectValidityChecking();
    ccdb->setCreatedNotAfter(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    ccdbApi.init(url);
    lut = o2::base::MatLayerCylSet::rectifyPtrFromFile(ccdb->get<o2::base::MatLayerCylSet>("GLO/Param/MatLUT"));
  }

  /// Basic selection of V0 candidates
  /// \param v0 is the v0 candidate
  /// \param collision is the current collision
  /// \return a bitmap with mass hypotesis if passes all cuts
  template <typename V0, typename Coll>
  inline uint8_t isSelectedV0(const V0& v0,  const Coll& collision, const std::array<int, 3>& dDaughtersIDs)
  {
    uint8_t isSelected{BIT(K0s) | BIT(Lambda) | BIT(AntiLambda)};
    // reject VOs that share daughters with D
    if (std::find(dDaughtersIDs.begin(), dDaughtersIDs.end(), v0.posTrackId()) != dDaughtersIDs.end() || std::find(dDaughtersIDs.begin(), dDaughtersIDs.end(), v0.negTrackId()) != dDaughtersIDs.end()) {
      return 0;
    }
    // eta of daughters
    if (std::fabs(v0.negativeeta()) > minV0dauEta || std::fabs(v0.positiveeta()) > minV0dauEta) { // cut all V0 daughters with |eta| > 1.
      return 0;
    }
    //minimum v0radius
    if (v0.v0radius() < minK0sLambdaRadius) {
      return 0;
    }
    // cosine of pointing angle
    auto v0CosinePa = v0.v0cosPA();
    if (v0CosinePa < minK0sLambdaCosinePa) {
      return 0;
    }
    // DCA V0 and V0 daughters to select for primary V0s
      if (v0.dcav0topv() > maxV0DCA || v0.dcaV0daughters() > maxV0dauDCA || std::fabs(v0.dcapostopv()) <minV0dauDCA || std::fabs(v0.dcanegtopv()) <minV0dauDCA ) { 
      return 0;
      }
    //mass hypotesis
    if (TESTBIT(isSelected, K0s) && std::fabs(v0.mK0Short() - MassK0) > deltaMassK0s) {
      CLRBIT(isSelected, K0s);
    }
    if (TESTBIT(isSelected, Lambda) && std::fabs(v0.mLambda() - MassLambda0) > deltaMassLambda) {
      CLRBIT(isSelected, Lambda);
    }
    if (TESTBIT(isSelected, AntiLambda) && std::fabs(v0.mAntiLambda() - MassLambda0) > deltaMassLambda) {
      CLRBIT(isSelected, AntiLambda);
    }
    return isSelected;
  }

  template <uint8_t DecayChannel, typename CCands>
  void runDataCreation(aod::Collision const& collision,
                       CCands const& candsD,
                       aod::V0Datas const& V0s,
                       aod::BCsWithTimestamps const& bcs)
  {
    // helpers for ReducedTables filling
    int indexHfReducedCollision = hfReducedCollision.lastIndex() + 1;
    // std::map where the key is the V0.globalIndex() and
    // the value is the V0 index in the table of the selected v0s
    std::map<int64_t, int64_t> selectedV0s;
    bool fillHfReducedCollision = false;
    auto bc = collision.bc_as<aod::BCsWithTimestamps>();
    if (runNumber != bc.runNumber()) {
      LOG(info) << ">>>>>>>>>>>> Current run number: " << runNumber;
      o2::parameters::GRPMagField* grpo = ccdb->getForTimeStamp<o2::parameters::GRPMagField>(ccdbPathGrpMag, bc.timestamp());
      if (grpo == nullptr) {
        LOGF(fatal, "Run 3 GRP object (type o2::parameters::GRPMagField) is not available in CCDB for run=%d at timestamp=%llu", bc.runNumber(), bc.timestamp());
      }
      o2::base::Propagator::initFieldFromGRP(grpo);
      // setMatLUT only after magfield has been initalized
      o2::base::Propagator::Instance()->setMatLUT(lut);
      runNumber = bc.runNumber();
    }

    for (const auto& candD : candsD) {
      // initialize variables depending on decay channel
      bool fillHfCandD = false;
      float invMassD;
      float massD;
      float massV0{0.};
      std::array<float, 3> pVecD;
      std::array<float, 3> secondaryVertexD;
      std::array<int, 3> prongIdsD;
      uint8_t v0_type;
      int8_t d_type;

      if constexpr (std::is_same<CCands, CandDstarFiltered>::value) {
        if (candD.signSoftPi() > 0)
          invMassD = candD.invMassDstar() - candD.invMassD0();
        else
          invMassD = candD.invMassAntiDstar() - candD.invMassD0Bar();
        massD = MassDStar;
        pVecD = candD.pVector();
        secondaryVertexD[0] = candD.xSecondaryVertexD0();
        secondaryVertexD[1] = candD.ySecondaryVertexD0();
        secondaryVertexD[2] = candD.zSecondaryVertexD0();
        prongIdsD[0] = candD.prong0Id();
        prongIdsD[1] = candD.prong1Id();
        prongIdsD[2] = candD.prongPiId();
        d_type = candD.signSoftPi() * TypeD::Dstar;
      } else if constexpr (std::is_same<CCands, CandsDplusFiltered>::value) {
        invMassD = hfHelper.invMassDplusToPiKPi(candD);
        massD = MassDPlus;
        pVecD = candD.pVector();
        secondaryVertexD[0] = candD.xSecondaryVertex();
        secondaryVertexD[1] = candD.ySecondaryVertex();
        secondaryVertexD[2] = candD.zSecondaryVertex();
        prongIdsD[0] = candD.prong0Id();
        prongIdsD[1] = candD.prong1Id();
        prongIdsD[2] = candD.prong2Id();
        d_type = candD.sign() * TypeD::Dplus;
      }   // else if

      // Loop on V0 candidates
      for (const auto& v0 : V0s) {
        v0_type = isSelectedV0(v0, collision,prongIdsD);
        //Apply selsection
        if (v0_type == 0){
          continue;
        }
        // propagate V0 to primary vertex (if enabled)
        std::array<float, 3> pVecV0 = {v0.px(), v0.py(), v0.pz()};
        if (propagateV0toPV) {
          std::array<float, 3> pVecV0Orig = {v0.px(), v0.py(), v0.pz()};
          std::array<float, 3> posVecV0 = {v0.x(), v0.y(), v0.z()};
          gpu::gpustd::array<float, 2> dcaInfo;
          auto trackParK0 = o2::track::TrackPar(posVecV0, pVecV0Orig, 0, true);
          trackParK0.setPID(o2::track::PID::K0);
          trackParK0.setAbsCharge(0);
          o2::base::Propagator::Instance()->propagateToDCABxByBz({collision.posX(), collision.posY(), collision.posZ()}, trackParK0, 2.f, matCorr, &dcaInfo);
          getPxPyPz(trackParK0, pVecV0);
        }
        float ptV0 = sqrt(pVecV0[0] * pVecV0[0] + pVecV0[1] * pVecV0[1]);\
        // fill histos
        registry.fill(HIST("hPtV0"), ptV0);
        registry.fill(HIST("hV0_type"), v0_type);
        if (TESTBIT(v0_type, K0s)){
          massV0 = MassK0;
          auto invMass2DV0 = RecoDecay::m2(std::array{pVecD, pVecV0}, std::array{massD, massV0});
          registry.fill(HIST("hMassK0s"), v0.mK0Short());
          switch (DecayChannel) {
          case DecayChannel::DstarV0:
            registry.fill(HIST("hMassDs1"), sqrt(invMass2DV0) - invMassD);
            break;
          case DecayChannel::DplusV0:
            registry.fill(HIST("hMassDsStar2"), sqrt(invMass2DV0) - invMassD);
            break;
          default:
            break;
          }
        }
        if (TESTBIT(v0_type, Lambda)){
          massV0 = MassLambda0;
          auto invMass2DV0 = RecoDecay::m2(std::array{pVecD, pVecV0}, std::array{massD, massV0});
          registry.fill(HIST("hMassLambda"), v0.mLambda());
          if(DecayChannel == DecayChannel::DplusV0){
            registry.fill(HIST("hMassXcRes"), sqrt(invMass2DV0) - invMassD);
          }
        }
        if(TESTBIT(v0_type, AntiLambda)){
          massV0 = MassLambda0;
          auto invMass2DV0 = RecoDecay::m2(std::array{pVecD, pVecV0}, std::array{massD, massV0});
          registry.fill(HIST("hMassLambda"), v0.mAntiLambda());
          if(DecayChannel == DecayChannel::DplusV0){
            registry.fill(HIST("hMassXcRes"), sqrt(invMass2DV0) - invMassD);
          }
        }
        // fill V0 table
        // if information on V0 already stored, go to next V0
        if (!selectedV0s.count(v0.globalIndex())) {
          hfCandV0(v0.posTrackId(), v0.negTrackId(),
                   indexHfReducedCollision,
                   v0.x(), v0.y(), v0.z(),
                   v0.mK0Short(), v0.mLambda(), v0.mAntiLambda(),
                   pVecV0[0], pVecV0[1], pVecV0[2],
                   v0.v0cosPA(),
                   v0.dcav0topv(),
                   v0.v0radius(),
                   v0_type);
          selectedV0s[v0.globalIndex()] = hfCandV0.lastIndex();
        }
        fillHfCandD = true;
      } // V0 loop

      if (fillHfCandD) { // fill candDplus table only once per D candidate, only if at least one V0 is found
        hfCandD(prongIdsD[0], prongIdsD[1], prongIdsD[2],
                indexHfReducedCollision,
                secondaryVertexD[0], secondaryVertexD[1], secondaryVertexD[2],
                invMassD,
                pVecD[0], pVecD[1], pVecD[2],
                d_type);
        fillHfReducedCollision = true;
        switch (DecayChannel) {
          case DecayChannel::DstarV0:
            registry.fill(HIST("hMassDstar"), invMassD);
            registry.fill(HIST("hPtDstar"), candD.pt());
            break;
          case DecayChannel::DplusV0:
            registry.fill(HIST("hMassDplus"), invMassD);
            registry.fill(HIST("hPtDplus"), candD.pt());
            break;
          default:
            break;
        }
        registry.fill(HIST("hPtDplus"), candD.pt());
        registry.fill(HIST("hD_type"), d_type);
      }
    } // candsD loop
    registry.fill(HIST("hEvents"), 1 + Event::Processed);
    if (!fillHfReducedCollision) {
      registry.fill(HIST("hEvents"), 1 + Event::NoDV0Selected);
      return;
    }
    registry.fill(HIST("hEvents"), 1 + Event::DV0Selected);
    // fill collision table if it contains a DPi pair a minima
    hfReducedCollision(collision.posX(), collision.posY(), collision.posZ(),
                       collision.covXX(), collision.covXY(), collision.covYY(),
                       collision.covXZ(), collision.covYZ(), collision.covZZ(),
                       0);
  } // run data creation

  void processDplusV0(aod::Collisions const& collisions,
                      CandsDplusFiltered const& candsDplus,
                      aod::TrackAssoc const& trackIndices,
                      aod::V0Datas const& V0s,
                      aod::BCsWithTimestamps const& bcs)
  {
    // handle normalization by the right number of collisions
    hfCollisionCounter(collisions.tableSize());

    for (const auto& collision : collisions) {
      auto thisCollId = collision.globalIndex();
      auto candsDThisColl = candsDplus.sliceBy(candsDplusPerCollision, thisCollId);
      auto V0sThisColl = V0s.sliceBy(candsV0PerCollision, thisCollId);
      runDataCreation<DecayChannel::DplusV0, CandsDplusFiltered>(collision, candsDThisColl, V0sThisColl, bcs);
    }
  }
  PROCESS_SWITCH(HfDataCreatorDV0Reduced, processDplusV0, "Process Dplus candidates without MC info and without ML info", true);

  void processDstarV0(aod::Collisions const& collisions,
                  CandDstarFiltered const& candsDstar,
                  aod::TrackAssoc const& trackIndices,
                  aod::V0Datas const& V0s,
                  aod::BCsWithTimestamps const& bcs)
  {
    // handle normalization by the right number of collisions
    hfCollisionCounter(collisions.tableSize());

    for (const auto& collision : collisions) {
      auto thisCollId = collision.globalIndex();
      auto candsDThisColl = candsDstar.sliceBy(candsDstarPerCollision, thisCollId);
      auto V0sThisColl = V0s.sliceBy(candsV0PerCollision, thisCollId);
      runDataCreation<DecayChannel::DstarV0, CandDstarFiltered>(collision, candsDThisColl, V0sThisColl, bcs);
    }
  }
  PROCESS_SWITCH(HfDataCreatorDV0Reduced, processDstarV0, "Process DStar candidates without MC info and without ML info", false);
}; // struct

WorkflowSpec defineDataProcessing(ConfigContext const& cfgc)
{
  return WorkflowSpec{adaptAnalysisTask<HfDataCreatorDV0Reduced>(cfgc)};
}
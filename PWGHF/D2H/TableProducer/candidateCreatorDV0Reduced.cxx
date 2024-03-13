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

/// \file candidateCreatorCharmResoReduced.cxx
/// \brief Reconstruction of Resonance candidates
///
/// \author Luca Aglietta <luca.aglietta@cern.ch>, Università degli Studi di Torino
#include "CommonConstants/PhysicsConstants.h"
#include "Framework/AnalysisTask.h"
#include "Framework/runDataProcessing.h"
#include "ReconstructionDataFormats/DCA.h"
#include "ReconstructionDataFormats/V0.h"

#include "Common/Core/trackUtilities.h"
#include "Common/DataModel/CollisionAssociationTables.h"

#include "PWGHF/D2H/DataModel/ReducedDataModel.h"

using namespace o2;
using namespace o2::aod;
using namespace o2::framework;
using namespace o2::framework::expressions;

enum Selections : uint8_t {
  NoSel = 0,
  DSel,
  V0Sel,
  NSelSteps
};
enum DecayChannel : uint8_t {
  Ds1toDstarK0s = 0,
  Dstar2toDplusK0s,
  XctoDplusLambda
};
enum V0Type : uint8_t {
  K0s = 0,
  Lambda,
  AntiLambda
};
const int nBins = 7;
constexpr double binsPt[nBins + 1] = {
  1.,
  2.,
  4.,
  6.,
  8.,
  12.,
  24.,
  50.};
auto vecBins = std::vector<double>{binsPt, binsPt + nBins + 1};

struct HfCandidateCreatorDV0Reduced {
  // Produces: Tables with resonance info
  Produces<aod::HfCandCharmReso> rowCandidateReso;
  // Configurables
  Configurable<double> invMassWindowD{"invMassWindowD", 0.5, "invariant-mass window for D candidates (GeV/c2)"};
  Configurable<double> invMassWindowV0{"invMassWindowV0", 0.5, "invariant-mass window for V0 candidates (GeV/c2)"};
  // QA switch
  Configurable<bool> activateQA{"activateQA", false, "Flag to enable QA histogram"};
  // Hist Axis
  Configurable<std::vector<double>> binsPt{"binsPt", std::vector<double>{vecBins}, "pT bin limits"};

  // Partition of V0 candidates based on v0Type
  Partition<aod::HfRedVzeros> candidatesK0s = aod::hf_reso_cand_reduced::v0Type == (uint8_t)1 || aod::hf_reso_cand_reduced::v0Type == (uint8_t)3 || aod::hf_reso_cand_reduced::v0Type == (uint8_t)5;
  Partition<aod::HfRedVzeros> candidatesLambda = aod::hf_reso_cand_reduced::v0Type == (uint8_t)2 || aod::hf_reso_cand_reduced::v0Type == (uint8_t)4;

  // Useful constants
  double massK0{0.};
  double massLambda{0.};
  double massDplus{0.};
  double massDstar{0.};

  // Histogram registry: if task make it with a THNsparse with all variables you want to save
  HistogramRegistry registry{"registry"};

  void init(InitContext const&)
  {
    for (const auto& value : vecBins) {
      LOGF(info, "bin limit %f", value);
    }
    const AxisSpec axisPt{(std::vector<double>)vecBins, "#it{p}_{T} (GeV/#it{c})"};
    // histograms
    registry.add("hMassDs1", "Ds1 candidates;m_{Ds1} - m_{D^{*}} (GeV/#it{c}^{2});entries", {HistType::kTH2F, {{100, 2.4, 2.7}, {(std::vector<double>)binsPt, "#it{p}_{T} (GeV/#it{c})"}}});
    registry.add("hMassDsStar2", "Ds^{*}2 candidates; Ds^{*}2 - m_{D^{#plus}} (GeV/#it{c}^{2}) ;entries", {HistType::kTH2F, {{100, 2.4, 2.7}, {(std::vector<double>)binsPt, "#it{p}_{T} (GeV/#it{c})"}}});
    registry.add("hMassXcRes", "XcRes candidates; XcRes - m_{D^{#plus}} (GeV/#it{c}^{2}) ;entries", {HistType::kTH2F, {{100, 2.9, 3.3}, {(std::vector<double>)binsPt, "#it{p}_{T} (GeV/#it{c})"}}});
    if (activateQA) {
      constexpr int kNBinsSelections = Selections::NSelSteps;
      std::string labels[kNBinsSelections];
      labels[Selections::NoSel] = "No selection";
      labels[Selections::DSel] = "D Candidates Selection";
      labels[Selections::V0Sel] = "D & V0 candidate Selection";
      static const AxisSpec axisSelections = {kNBinsSelections, 0.5, kNBinsSelections + 0.5, ""};
      registry.add("hSelections", "Selections", {HistType::kTH1F, {axisSelections}});
      for (int iBin = 0; iBin < kNBinsSelections; ++iBin) {
        registry.get<TH1>(HIST("hSelections"))->GetXaxis()->SetBinLabel(iBin + 1, labels[iBin].data());
      }
    }

    massK0 = o2::constants::physics::MassK0Short;
    massLambda = o2::constants::physics::MassLambda;
    massDplus = o2::constants::physics::MassDPlus;
    massDstar = o2::constants::physics::MassDStar;
  }

  template <DecayChannel channel, typename D>
  bool isDSelected(D const& candD)
  {
    float massD{0.};
    // slection on D candidate mass
    if (DecayChannel == DecayChannel::Dstar2toDplusK0s || DecayChannel == DecayChannel::XctoDplusLambda) {
      massD = massDplus;
    } else if (DecayChannel == DecayChannel::Ds1toDstarK0s) {
      massD = massDstar;
    }
    if (std::fabs(candD.invMass() - massD) > invMassWindowD) {
      return false;
    }
    return true;
  }

  template <uint8_t DecayChannel, typename D, typename V>
  bool isV0Selected(V const& candV0, D const& candD)
  {
    float massV0{0.};
    float invMassV0{0.};
    // slection on V0 candidate mass
    if (DecayChannel == DecayChannel::Dstar2toDplusK0s || DecayChannel == DecayChannel::Ds1toDstarK0s) {
      massV0 = massK0;
      invMassV0 = candV0.invMassK0s();
    } else if (DecayChannel == DecayChannel::XctoDplusLambda) {
      massV0 = massLambda;
      if (candD.dType() > 0) {
        invMassV0 = candV0.invMassLambda();
      } else {
        invMassV0 = candV0.invMassAntiLambda();
      }
    }
    if (std::fabs(invMassV0 - massV0) > invMassWindowV0) {
      return false;
    }
    return true;
  }

  template <DecayChannel channel, typename C, typename D, typename V>
  void runCandidateCreation(C const& collisions,
                            D const& candsD,
                            V const& candsV0)
  {
    // loop on D candidates
    for (const auto& candD : candsD) {
      // selection of D candidates
      if (activateQA) {
        registry.fill(HIST("hSelections"), 1);
      }
      if (!isDSelected<DecayChannel>(candD)) {
        continue;
      }
      if (activateQA) {
        registry.fill(HIST("hSelections"), 1 + Selections::DSel);
      }
      float invMassD = candD.invMass();
      std::array<float, 3> pVecD = {candD.px(), candD.py(), candD.pz()};
      float ptD = RecoDecay::pt(pVecD);
      ;
      // loop on V0 candidates
      bool already_counted{false};
      for (const auto& candV0 : candsV0) {
        if (!isV0Selected<DecayChannel>(candV0, candD)) {
          continue;
        }
        if (activateQA && !already_counted) {
          registry.fill(HIST("hSelections"), 1 + Selections::V0Sel);
          already_counted = true;
        }
        float invMass2Reso{0.};
        float invMassV0{0.};
        std::array<float, 3> pVecV0 = {candV0.px(), candV0.py(), candV0.pz()};
        float ptV0 = RecoDecay::pt(pVecV0);
        float ptReso = RecoDecay::pt(RecoDecay::sumOfVec(pVecV0, pVecD));
        switch (DecayChannel) {
          case DecayChannel::Ds1toDstarK0s:
            invMassV0 = candV0.invMassK0s();
            invMass2Reso = RecoDecay::m2(std::array{pVecD, pVecV0}, std::array{massDstar, massK0});
            registry.fill(HIST("hMassDs1"), sqrt(invMass2Reso), ptReso);
            break;
          case DecayChannel::Dstar2toDplusK0s:
            invMassV0 = candV0.invMassK0s();
            invMass2Reso = RecoDecay::m2(std::array{pVecD, pVecV0}, std::array{massDplus, massK0});
            registry.fill(HIST("hMassDsStar2"), sqrt(invMass2Reso), ptReso);
            break;
          case DecayChannel::XctoDplusLambda:
            if (candD.dType() > 0) {
              invMassV0 = candV0.invMassLambda();
            } else {
              invMassV0 = candV0.invMassAntiLambda();
            }
            invMass2Reso = RecoDecay::m2(std::array{pVecD, pVecV0}, std::array{massDplus, massLambda});
            registry.fill(HIST("hMassXcRes"), sqrt(invMass2Reso), ptReso);
            break;
          default:
            break;
        }
        // Filling Output table
        rowCandidateReso(collisions.globalIndex(),
                         sqrt(invMass2Reso),
                         ptReso,
                         invMassD,
                         ptD,
                         invMassV0,
                         ptV0,
                         candV0.cpa(),
                         candV0.dca(),
                         candV0.radius());
      }
    }
  } // main function

  void processDstar2ToDplusK0s(aod::HfRedCollisions::iterator const& collision,
                               aod::HfRed3PrNoTrks const& candsD,
                               aod::HfRedVzeros const& candsV0)
  {
    runCandidateCreation<DecayChannel::Dstar2toDplusK0s>(collision, candsD, candidatesK0s);
  }
  PROCESS_SWITCH(HfCandidateCreatorDV0Reduced, processDstar2toDplusK0s, "Process Dplus candidates without MC info and without ML info", true);

  void processDs1toDstarK0s(aod::HfRedCollisions::iterator const& collision,
                            aod::HfRed3PrNoTrks const& candsD,
                            aod::HfRedVzeros const& candsV0)
  {
    runCandidateCreation<DecayChannel::Ds1toDstarK0s>(collision, candsD, candidatesK0s);
  }
  PROCESS_SWITCH(HfCandidateCreatorDV0Reduced, processDs1toDstarK0s, "Process Dplus candidates without MC info and without ML info", false);

  void processXctoDplusLambda(aod::HfRedCollisions::iterator const& collision,
                              aod::HfRed3PrNoTrks const& candsD,
                              aod::HfRedVzeros const& candsV0)
  {
    runCandidateCreation<DecayChannel::XctoDplusLambda>(collision, candsD, candidatesLambda);
  }
  PROCESS_SWITCH(HfCandidateCreatorDV0Reduced, processXctoDplusLambda, "Process Dplus candidates without MC info and without ML info", false);
}; // struct

WorkflowSpec defineDataProcessing(ConfigContext const& cfgc)
{
  return WorkflowSpec{adaptAnalysisTask<HfCandidateCreatorDV0Reduced>(cfgc)};
}

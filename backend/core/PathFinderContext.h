/*
 Copyright (c) 2012 Petr Koupy

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <vector>

#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>

#include "global_types.h"
#include "fingerprint_selectors.h"
#include "simcoeff_selectors.h"
#include "dimred_selectors.h"
#include "chemoper_selectors.h"
#include "scaffold_selectors.hpp"

#include "MolpherParam.h"
#include "MolpherMolecule.h"
#include "IterationSnapshot.h"

struct PathFinderContext
{
    static void ContextToSnapshot(const PathFinderContext &ctx, IterationSnapshot &snp);
    static void SnapshotToContext(const IterationSnapshot &snp, PathFinderContext &ctx);

    static void ContextToLightSnapshot(const PathFinderContext &ctx, IterationSnapshot &snp);

    bool ScaffoldMode() const;

    JobId jobId;
    unsigned int iterIdx;
    unsigned int elapsedSeconds;

    FingerprintSelector fingerprintSelector;
    SimCoeffSelector simCoeffSelector;
    DimRedSelector dimRedSelector;
    std::vector<ChemOperSelector> chemOperSelectors;

    MolpherParam params;

    MolpherMolecule source;
    MolpherMolecule target;
    std::vector<MolpherMolecule> decoys;

    typedef tbb::concurrent_hash_map<std::string, MolpherMolecule> CandidateMap;
    typedef tbb::concurrent_hash_map<std::string, unsigned int> MorphDerivationMap;
    typedef tbb::concurrent_vector<std::string> PrunedMoleculeVector;
    typedef tbb::concurrent_hash_map<std::string, std::string> ScaffoldSmileMap;

    CandidateMap candidates;
    
    /**
     * activity data information and types
     */
    typedef std::vector<std::string> ConcStringVector;
    //typedef tbb::concurrent_vector<double> ConcDoubleVector;
    typedef std::vector<double> ConcDoubleVector;
    //typedef tbb::concurrent_vector<std::pair<double, double> > ConcDoublePairVector;
    typedef std::vector<std::pair<double, double> > ConcDoublePairVector;
    //typedef tbb::concurrent_vector<bool> ConcBoolVector;
    typedef std::vector<bool> ConcBoolVector;
    //typedef tbb::concurrent_vector<std::vector<double> > ConcDoubleMatrix;
    typedef std::vector<std::vector<double> > ConcDoubleMatrix;
    CandidateMap actives;
    ConcStringVector activesIDs;
    ConcDoubleVector etalonValues;
    ConcDoubleMatrix activesDescriptors;
    ConcDoublePairVector normalizationCoefficients;
    ConcStringVector relevantDescriptorNames;
//    ConcBoolVector relevantDescriptorIndices;
    bool activityMorphingInitialized;
    
    /**
     * activity data files information
     */
    std::string inputActivityDataDir;
    std::string activesSDFFile;
//    std::string inactivesSDFFile;
    std::string proteinTargetName;
    std::string activesDescriptorsFile;
//    std::string inactivesDescriptorsFile;
    std::string descriptorDataFileSuffix;
    std::string analysisResultsSuffix;
    
    MorphDerivationMap morphDerivations;
    PrunedMoleculeVector prunedDuringThisIter;
    
    MolpherMolecule tempSource;
    ScaffoldSelector scaffoldSelector;
    std::vector<MolpherMolecule> pathMolecules;
    ScaffoldSmileMap pathScaffoldMolecules;
    ScaffoldSmileMap candidateScaffoldMolecules;

    MolpherMolecule substructure;
};
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
#include <map>
#include <fstream>

#include <boost/version.hpp>
#include <boost/cstdint.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/tracking.hpp>
#include <boost/serialization/level.hpp>
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

#include "fingerprint_selectors.h"
#include "simcoeff_selectors.h"
#include "dimred_selectors.h"
#include "chemoper_selectors.h"
#include "scaffold_selectors.hpp"

#include "MolpherParam.h"
#include "MolpherMolecule.h"
#include "activity_data_processing.h"
#include "CSV.h"

struct IterationSnapshot
{
    IterationSnapshot() :
        jobId(0),
        iterIdx(0),
        elapsedSeconds(0),
        activesSDFFile(""),
//        inactivesSDFFile(""),
        inputActivityDataDir(""),
        descriptorDataFileSuffix("_padel_descriptors"),
        analysisResultsSuffix("_results"),
        saveDataAsCSVs(false),
        activityMorphingInitialized(false)
    {
        fingerprintSelector = DEFAULT_FP;
        simCoeffSelector = DEFAULT_SC;
        dimRedSelector = DEFAULT_DR;
        scaffoldSelector = SF_NONE;
    }

    friend class boost::serialization::access;
    template<typename Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        // usage of BOOST_SERIALIZATION_NVP macro enable us to use xml serialisation
        ar & BOOST_SERIALIZATION_NVP(jobId);
        ar & BOOST_SERIALIZATION_NVP(iterIdx); 
        ar & BOOST_SERIALIZATION_NVP(elapsedSeconds);
        ar & BOOST_SERIALIZATION_NVP(fingerprintSelector);
        ar & BOOST_SERIALIZATION_NVP(simCoeffSelector);
        ar & BOOST_SERIALIZATION_NVP(dimRedSelector);
        ar & BOOST_SERIALIZATION_NVP(chemOperSelectors);
        ar & BOOST_SERIALIZATION_NVP(params);
        ar & BOOST_SERIALIZATION_NVP(source);
        ar & BOOST_SERIALIZATION_NVP(target);
        ar & BOOST_SERIALIZATION_NVP(decoys);
        ar & BOOST_SERIALIZATION_NVP(candidates);
        ar & BOOST_SERIALIZATION_NVP(morphDerivations);
        ar & BOOST_SERIALIZATION_NVP(prunedDuringThisIter);
        ar & BOOST_SERIALIZATION_NVP(tempSource);
        ar & BOOST_SERIALIZATION_NVP(scaffoldSelector);
        ar & BOOST_SERIALIZATION_NVP(pathMolecules);
        ar & BOOST_SERIALIZATION_NVP(pathScaffoldMolecules);
        ar & BOOST_SERIALIZATION_NVP(candidateScaffoldMolecules);
        
        ar & BOOST_SERIALIZATION_NVP(inputActivityDataDir);
        ar & BOOST_SERIALIZATION_NVP(activesSDFFile);
//        ar & BOOST_SERIALIZATION_NVP(inactivesSDFFile);
        ar & BOOST_SERIALIZATION_NVP(proteinTargetName);
        ar & BOOST_SERIALIZATION_NVP(activesDescriptorsFile);
//        ar & BOOST_SERIALIZATION_NVP(inactivesDescriptorsFile);
        ar & BOOST_SERIALIZATION_NVP(descriptorDataFileSuffix);
        ar & BOOST_SERIALIZATION_NVP(analysisResultsSuffix);
        ar & BOOST_SERIALIZATION_NVP(activityMorphingInitialized);
        ar & BOOST_SERIALIZATION_NVP(relevantDescriptorNames);
        ar & BOOST_SERIALIZATION_NVP(actives);
        ar & BOOST_SERIALIZATION_NVP(activesIDs);
        ar & BOOST_SERIALIZATION_NVP(etalonValues);
//        ar & BOOST_SERIALIZATION_NVP(actives);
        ar & BOOST_SERIALIZATION_NVP(normalizationCoefficients);
    }

    bool IsValid()
    {
        bool decoysValid = true;
        std::vector<MolpherMolecule>::iterator it;
        for (it = decoys.begin(); it != decoys.end(); ++it) {
            if (!it->IsValid()) {
                decoysValid = false;
                break;
            }
        }

        bool scaffoldsValid = !ScaffoldMode() ||
            (tempSource.IsValid() && !pathMolecules.empty());
        
        if (params.activityMorphing) {
            bool activityMorphingValid = !activesSDFFile.empty();//&& !inactivesSDFFile.empty();
            activityMorphingValid = params.startMolMaxCount >= 0;
            fs::path data_dir(inputActivityDataDir);
            if (activityMorphingValid && fs::exists(data_dir)) {
                fs::path actives(inputActivityDataDir + activesSDFFile);
//                fs::path inactives(inputActivityDataDir + inactivesSDFFile);
                fs::path desc_actives(inputActivityDataDir + activesDescriptorsFile);
//                fs::path desc_inactives(inputActivityDataDir + inactivesDescriptorsFile);
                fs::path analysis_results(inputActivityDataDir + proteinTargetName + analysisResultsSuffix);
                
                activityMorphingValid = fs::exists(actives) 
//                        && fs::exists(inactives)
                        && fs::exists(desc_actives)
//                        && fs::exists(desc_inactives)
                        && fs::exists(analysis_results);
            } else {
                activityMorphingValid = false;
            }
            
            return (!chemOperSelectors.empty()) &&
            params.IsValid() &&
            decoysValid &&
            activityMorphingValid;
        }

        return (!chemOperSelectors.empty()) &&
            params.IsValid() &&
            source.IsValid() &&
            target.IsValid() &&
            (source.smile != target.smile) &&
            decoysValid &&
            scaffoldsValid;
    }

    bool ScaffoldMode() const
    {
        return ((int)scaffoldSelector != SF_NONE);
    }
    
    bool IsActivityMorphingOn() const {
        return params.activityMorphing && !activityMorphingInitialized;
    }
  
    typedef std::map<std::string, MolpherMolecule> CandidateMap;
    typedef std::map<std::string, boost::uint32_t> MorphDerivationMap;
    typedef std::vector<std::string> PrunedMoleculeVector;
    typedef std::map<std::string, std::string> ScaffoldSmileMap;

    void saveEtalonDistancesCSV(CSVparse::CSV& descriptors_CSV, const std::string& out_path) {
        CSVparse::CSV output;
        const std::vector<std::string> &ids = descriptors_CSV.getStringData("Name");
        std::vector<double> etal_dists;
        for (unsigned int idx = 0; idx < descriptors_CSV.getRowCount(); idx++) {
            MolpherMolecule mm("", ids[idx]);
            mm.descriptorsFilePath = inputActivityDataDir + proteinTargetName + "_actives_all_padel_descriptors";
            mm.relevantDescriptorNames = relevantDescriptorNames;

            for (std::vector<std::string>::iterator it = relevantDescriptorNames.begin(); it != relevantDescriptorNames.end(); it++) {
                mm.descriptorValues.push_back(descriptors_CSV.getFloatData(*it)[idx]);
            }

            mm.normalizeDescriptors(normalizationCoefficients);
            mm.ComputeEtalonDistances(etalonValues);

            etal_dists.push_back(mm.distToEtalon);
        }
        output.addStringData("PMID", ids);
        output.addFloatData("DistToEtalon", etal_dists);
        output.write(out_path);
    }
       
    void PrepareActivityData() {        
        // read the selected features
        CSVparse::CSV analysis_results_CSV(inputActivityDataDir + proteinTargetName + analysisResultsSuffix, ";", "NA", true, true);
        const std::vector<string> &desc_names = analysis_results_CSV.getHeader();
        unsigned int rejected_row_idx = analysis_results_CSV.getRowIdx("rejected");
        for (std::vector<string>::const_iterator it = desc_names.begin(); it != desc_names.end(); it++) {
            if (analysis_results_CSV.getFloatData(*it)[rejected_row_idx] == 1) {
                relevantDescriptorNames.push_back(*it);
            }
        }
        
        // load data about actives
        CSVparse::CSV actives_descs_CSV(inputActivityDataDir + activesDescriptorsFile, ",", "");
        activesIDs = actives_descs_CSV.getStringData("Name");
        for (std::vector<string>::const_iterator id = activesIDs.begin(); id != activesIDs.end(); id++) {
            activesIDsSet.insert(*id);
        }
        
        // load data about decoys
        CSVparse::CSV decoys_descs_CSV(inputActivityDataDir + proteinTargetName + "_decoys_padel_descriptors", ",", "");
        
        // extract only the data for selected features
        std::vector<std::vector<double> > all_mols;
        std::vector<std::vector<double> > actives;
        adp::readRelevantData(actives_descs_CSV, relevantDescriptorNames, all_mols);
        if (saveDataAsCSVs) {
            CSVparse::CSV actives_CSV;
            adp::readRelevantData(actives_descs_CSV, relevantDescriptorNames, actives, actives_CSV);
            actives_CSV.write("Results/active_mols_selected_feats.csv");
            CSVparse::CSV decoys_CSV;
            adp::readRelevantData(decoys_descs_CSV, relevantDescriptorNames, all_mols, decoys_CSV);
            decoys_CSV.write("Results/decoy_mols_selected_feats.csv");
        } else {
            adp::readRelevantData(actives_descs_CSV, relevantDescriptorNames, actives);
            adp::readRelevantData(decoys_descs_CSV, relevantDescriptorNames, all_mols);
        }
      
        // compute normalization (feature scaling) coefficients and normalize all data
        adp::normalizeData(all_mols, 0, 1000, normalizationCoefficients);
        adp::normalizeData(actives, normalizationCoefficients);
        
        // use the scaled data to compute etalon values
        adp::computeEtalon(actives, etalonValues);
        
        // read structeres of the active molecules
        std::vector<std::string> actives_smiles;
        adp::readPropFromSDF(inputActivityDataDir + activesSDFFile, "PUBCHEM_OPENEYE_CAN_SMILES", actives_smiles);
        
        // create MolpherMolecule from each active
        std::vector<std::string>::size_type idx = 0;
        for (std::vector<std::string>::iterator it = actives_smiles.begin(); it != actives_smiles.end(); it++, idx++) {
            MolpherMolecule mm(*it, activesIDs[idx], actives[idx], inputActivityDataDir + activesDescriptorsFile, relevantDescriptorNames);
            mm.ComputeEtalonDistances(etalonValues);
            this->actives.insert(std::make_pair<std::string, MolpherMolecule>(*it, mm));
        }

        // read test actives and save as MolpherMolecules
        CSVparse::CSV all_actives(
                inputActivityDataDir + proteinTargetName
                + "_actives_all.smi", "\t", "", false, false);
        CSVparse::CSV all_actives_descriptors(inputActivityDataDir + proteinTargetName + "_actives_all_padel_descriptors", ",", "");
        const std::vector<std::string> &ids = all_actives.getStringData(1);
        const std::vector<std::string> &smiles = all_actives.getStringData(0);
        std::ofstream outfile("Results/test_mols_dists.csv");
        if (saveDataAsCSVs) {
            outfile << "PMID;DistToEtalon" << std::endl;
        }
        for (unsigned int idx = 0; idx < all_actives.getRowCount(); idx++) {
            if (activesIDsSet.find(ids[idx]) == activesIDsSet.end()) {
                MolpherMolecule mm(smiles[idx], ids[idx]);
                mm.descriptorsFilePath = inputActivityDataDir + proteinTargetName + "_actives_all_padel_descriptors";
                mm.relevantDescriptorNames = relevantDescriptorNames;
                
                for (std::vector<std::string>::iterator it = relevantDescriptorNames.begin(); it != relevantDescriptorNames.end(); it++) {
                    mm.descriptorValues.push_back(all_actives_descriptors.getFloatData(*it)[idx]);
                }
                
                mm.normalizeDescriptors(normalizationCoefficients);
                mm.ComputeEtalonDistances(etalonValues);
                
                if (saveDataAsCSVs) {
                    outfile << mm.id << ";" << mm.distToEtalon << std::endl;
                }
                
                testActives.insert(std::make_pair<std::string, MolpherMolecule>(smiles[idx], mm));
            }
        }
        outfile.close();

        // save etalon distances for actives and decoys
        if (saveDataAsCSVs) {
            saveEtalonDistancesCSV(decoys_descs_CSV, "Results/decoy_mols_dists.csv");
            saveEtalonDistancesCSV(actives_descs_CSV, "Results/train_mols_dists.csv");
        }
        
        activityMorphingInitialized = true;
    }

    /**
     * Job id.
     */
    boost::uint32_t jobId;

    /**
     * Iteration id.
     */
    boost::uint32_t iterIdx;

    /**
     * Total time spend by working on this instance of snapshot.
     */
    boost::uint32_t elapsedSeconds;

    /**
     * Fingerprint used in algorithm.
     * @see FingerprintSelector
     */
    boost::int32_t fingerprintSelector;

    /**
     * Similarity coef used in algorithm.
     * @see SimCoeffSelector
     */
    boost::int32_t simCoeffSelector;

    /**
     * Id of used location computing algorithm.
     * @see DimRedSelector
     */
    boost::int32_t dimRedSelector;

    /**
     * Vector or used morphing operator. Determine
     * possible morphing operation applied during generating
     * new morphs.
     * @see ChemOperSelector
     */
    std::vector<boost::int32_t> chemOperSelectors;

    /**
     * Parameters for morphing algorithm.
     */
    MolpherParam params;

    /**
     * Source molecule.
     */
    MolpherMolecule source;

    /**
     * Target molecule.
     */
    MolpherMolecule target;

    /**
     * Decoys used during exploration of chemical space.
     */
    std::vector<MolpherMolecule> decoys;

    /**
     * Candidate molecules ie. molecule storage.
     */
    CandidateMap candidates;
    CandidateMap actives;
    CandidateMap testActives;
    std::vector<std::string> activesIDs;
    std::set<std::string> activesIDsSet;
    std::vector<double> etalonValues;
//    std::vector<std::vector<double> > actives;
    std::vector<std::pair<double, double> > normalizationCoefficients;
    std::vector<std::string> relevantDescriptorNames;
    bool saveDataAsCSVs;
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

    /// variables used for scaffold hopping
    MolpherMolecule tempSource;
    boost::int32_t scaffoldSelector;
    std::vector<MolpherMolecule> pathMolecules;
    ScaffoldSmileMap pathScaffoldMolecules;
    ScaffoldSmileMap candidateScaffoldMolecules;
};

// add information about version to archive
//BOOST_CLASS_IMPLEMENTATION(IterationSnapshot, object_class_info)
// turn off versioning
BOOST_CLASS_IMPLEMENTATION(IterationSnapshot, object_serializable)
// turn off tracking
BOOST_CLASS_TRACKING(IterationSnapshot, track_never)
// specify version
//BOOST_CLASS_VERSION(IterationSnapshot, 1)

struct IterationSnapshotProxy
{
    IterationSnapshotProxy() :
        jobId(0),
        iterIdx(0)
    {
    }

    IterationSnapshotProxy(std::string &storage,
        boost::uint32_t jobId, boost::uint32_t iterIdx
        ) :
        storage(storage),
        jobId(jobId),
        iterIdx(iterIdx)
    {
    }

    std::string storage;
    boost::uint32_t jobId;
    boost::uint32_t iterIdx;
};

/*
 * File:   PathFinderActivity.h
 * Author: Krwemil
 *
 * Created on 11. říjen 2014, 15:27
 */

#include <cassert>
#include <cmath>
#include <cfloat>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdlib>

#include <boost/filesystem.hpp>

#include <tbb/task_scheduler_init.h>
#include <tbb/tbb_exception.h>
#include <tbb/partitioner.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <GraphMol/RDKitBase.h>
#include <GraphMol/SmilesParse/SmilesParse.h>
#include <GraphMol/SmilesParse/SmilesWrite.h>
#include <GraphMol/Substruct/SubstructMatch.h>

#include "inout.h"
#include "auxiliary/SynchRand.h"
#include "coord/ReducerFactory.h"
#include "chem/morphing/Morphing.hpp"
#include "../chem/scaffold/Scaffold.hpp"
#include "../chem/scaffold/ScaffoldDatabase.hpp"
#include "../chem/SimCoefCalculator.hpp"
#include "JobManager.h"
#include "PathFinderActivity.h"
#include "chem/morphing/ReturnResults.hpp"

#include "descriptor/DescriptorSource.hpp"

PathFinderActivity::PathFinderActivity(
        tbb::task_group_context *tbbCtx, JobManager *jobManager, int threadCnt
        ) :
mTbbCtx(tbbCtx),
mJobManager(jobManager),
mThreadCnt(threadCnt) {
}

PathFinderActivity::~PathFinderActivity() {
}

bool PathFinderActivity::Cancelled() {
    return mTbbCtx->is_group_execution_cancelled();
}

PathFinderActivity::FindLeaves::FindLeaves(MoleculeVector &leaves) :
mLeaves(leaves) {
}

void PathFinderActivity::FindLeaves::operator()(
        const PathFinderContext::CandidateMap::range_type &candidates) const {
    PathFinderContext::CandidateMap::iterator it;
    for (it = candidates.begin(); it != candidates.end(); it++) {
        if (!it->second.parentSmile.empty()) {
            it->second.itersWithoutDistImprovement++;
        }
        bool isLeaf = it->second.descendants.empty();
        if (isLeaf) {
            mLeaves.push_back(it->second);
        }
    }
}

PathFinderActivity::FindNextBag::FindNextBag(MoleculeVector &nextBag) :
mNext(nextBag) {
}

void PathFinderActivity::FindNextBag::operator()(
        const PathFinderContext::CandidateMap::range_type &candidates) const {
    PathFinderContext::CandidateMap::iterator it;
    for (it = candidates.begin(); it != candidates.end(); it++) {
        if (!it->second.decayed) {
            it->second.itersWithoutDistImprovement++;
            it->second.itersFresh++;
            mNext.push_back(it->second);
        }
    }
}

PathFinderActivity::CollectMorphs::CollectMorphs(MoleculeVector &morphs) :
mMorphs(morphs) {
    mCollectAttemptCount = 0;
}

void PathFinderActivity::CollectMorphs::operator()(const MolpherMolecule &morph) {
    ++mCollectAttemptCount; // atomic
    SmileSet::const_accessor dummy;
    if (mDuplicateChecker.insert(dummy, morph.smile)) {
        mMorphs.push_back(morph);
    } else {
        // ignore duplicate
    }
}

unsigned int PathFinderActivity::CollectMorphs::WithdrawCollectAttemptCount() {
    unsigned int ret = mCollectAttemptCount;
    mCollectAttemptCount = 0;
    return ret;
}

void MorphCollector2(MolpherMolecule *morph, void *functor) {
    PathFinderActivity::CollectMorphs *collect =
            (PathFinderActivity::CollectMorphs *) functor;
    (*collect)(*morph);
}

// return true if "a" is closes to target then "b"

bool PathFinderActivity::CompareMorphs::operator()(
        const MolpherMolecule &a, const MolpherMolecule &b) const {
    /* Morphs are rated according to their proximity to the connecting line
     between their closest decoy and the target (i.e. sum of both distances is
     minimal on the connecting line between decoy and target). When sums for
     both morphs are equal, it is possible (but not necessary) that both
     morphs lie on the same connecting line. In that case, morphs are
     rated only according to their proximity to the target. Such comparison
     should allow convergence to the target even in the late stages of the
     algorithm when majority of morphs lie on the connecting line between
     decoy closest to the target and the target itself. */

    double aSum = a.distToTarget + a.distToClosestDecoy;
    double bSum = b.distToTarget + b.distToClosestDecoy;

    bool approximatelyEqual = (
            fabs(aSum - bSum) <= (32 * DBL_EPSILON * fmax(fabs(aSum), fabs(bSum))));

    if (approximatelyEqual) {
        return a.distToTarget < b.distToTarget;
    } else {
        return aSum < bSum;
    }
}

//PathFinderActivity::FilterMorphs::FilterMorphs(PathFinderContext &ctx,
//        size_t globalMorphCount, MoleculeVector &morphs, std::vector<bool> &survivors
//        ) :
//mCtx(ctx),
//mGlobalMorphCount(globalMorphCount),
//mMorphs(morphs),
//mMorphCount(morphs.size()),
//mSurvivors(survivors) {
//    assert(mMorphs.size() == mSurvivors.size());
//}

PathFinderActivity::FilterMorphs::FilterMorphs(PathFinderContext &ctx,
        size_t globalMorphCount, MoleculeVector &morphs, std::vector<bool> &survivors
        ) :
mCtx(ctx),
mGlobalMorphCount(globalMorphCount),
mMorphs(morphs),
mMorphCount(morphs.size()),
mSurvivors(survivors){
    assert(mMorphs.size() == mSurvivors.size());
}

void PathFinderActivity::FilterMorphs::operator()(const tbb::blocked_range<size_t> &r) const {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {

//        double acceptProbability = 1.0;
//        bool isTarget = !mCtx.ScaffoldMode() ?
//                (mMorphs[idx].smile.compare(mCtx.target.smile) == 0) :
//                (mMorphs[idx].scaffoldSmile.compare(mCtx.target.scaffoldSmile) == 0);
//        if (idx >= mCtx.params.cntCandidatesToKeep && !isTarget) {
//            acceptProbability =
//                    0.25 - (idx - mCtx.params.cntCandidatesToKeep) /
//                    ((mGlobalMorphCount - mCtx.params.cntCandidatesToKeep) * 4.0);
//        }

//        bool mightSurvive =
//                SynchRand::GetRandomNumber(0, 99) < (int) (acceptProbability * 100);
//        if (mightSurvive) {

        if (true) {
            bool isDead = false;
            bool badWeight = false;
            bool badSascore = false;
            bool alreadyExists = false;
            bool alreadyTriedByParent = false;
            bool tooManyProducedMorphs = false;
            bool isTooFarFromEtalon = false; // not used at the moment

            // Tests are ordered according to their cost.
            // Added test for SAScore

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

            if (!isDead) {
                badWeight =
                        (mMorphs[idx].molecularWeight <
                        mCtx.params.minAcceptableMolecularWeight) ||
                        (mMorphs[idx].molecularWeight >
                        mCtx.params.maxAcceptableMolecularWeight);
                if (badWeight) {
                    std::stringstream ss;
                        ss << "\tBad weight: " << mMorphs[idx].smile << " : "
                                << mMorphs[idx].molecularWeight;
                    SynchCout( ss.str() );
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

            if (!isDead) {
                if (mCtx.params.useSyntetizedFeasibility) {
                    badSascore = mMorphs[idx].sascore > 6.0; // questionable, it is recommended value from Ertl
                    // in case of badSascore print message
                    if (badSascore) {
                        std::stringstream ss;
                        ss << "\tBad sasscore: " << mMorphs[idx].smile << " : "
                                << mMorphs[idx].sascore;
                        SynchCout( ss.str() );
                    }
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

            if (!isDead) {
                if (!mCtx.ScaffoldMode()) {
                    PathFinderContext::CandidateMap::const_accessor dummy;
                    if (mCtx.candidates.find(dummy, mMorphs[idx].smile)) {
                        alreadyExists = true;
                    }
                } else {
                    PathFinderContext::ScaffoldSmileMap::const_accessor dummy;
                    bool isInCandidates = mCtx.candidateScaffoldMolecules.find(dummy, mMorphs[idx].scaffoldSmile);
                    dummy.release();
                    bool isOnPath = mCtx.pathScaffoldMolecules.find(dummy, mMorphs[idx].scaffoldSmile);
                    if (isInCandidates ||
                            (isOnPath && mMorphs[idx].scaffoldSmile.compare(mCtx.target.scaffoldSmile) != 0)) {
                        alreadyExists = true;
                    }
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

            if (!isDead) {
                PathFinderContext::CandidateMap::const_accessor ac;
                if (mCtx.candidates.find(ac, mMorphs[idx].parentSmile)) {
                    alreadyTriedByParent = (
                            ac->second.historicDescendants.find(mMorphs[idx].smile)
                            !=
                            ac->second.historicDescendants.end());
                } else {
                    // Missing parent for a molecule.
                    assert(false);
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

            if (!isDead) {
                PathFinderContext::MorphDerivationMap::const_accessor ac;
                if (mCtx.morphDerivations.find(ac, mMorphs[idx].smile)) {
                    tooManyProducedMorphs =
                            (ac->second > mCtx.params.cntMaxMorphs);
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

//            if (!isDead) {
//                isTooFarFromEtalon = mMorphs[idx].distToEtalon > mCtx.params.maxAcceptableEtalonDistance
//                            || mMorphs[idx].distToEtalon == DBL_MAX;
//            }
//
//            isDead = (badWeight || badSascore || alreadyExists ||
//                    alreadyTriedByParent || tooManyProducedMorphs || isTooFarFromEtalon);

            mSurvivors[idx] = !isDead;
        }
    }

}

PathFinderActivity::MOOPFilter::MOOPFilter(
        MoleculeVector &morphs, std::vector<bool> &survivors,
        std::vector<bool> &next
        ) :
mMorphs(morphs),
mMorphCount(morphs.size()),
mSurvivors(survivors),
mNext(next){
    assert(mMorphs.size() == mSurvivors.size());
    assert(mMorphs.size() == mNext.size());
}

void PathFinderActivity::MOOPFilter::operator()(const tbb::blocked_range<size_t> &r) const {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {
        if (mNext[idx]) {
            bool isNotOptimal = false;
            MolpherMolecule first = mMorphs[idx];
            for (size_t second_idx = 0; second_idx != mMorphCount; ++second_idx) {
                MolpherMolecule second = mMorphs[second_idx];
                if ((first.id.compare(second.id) != 0) && mNext[second_idx]) {
                    std::vector<double>::size_type all_features_count = first.etalonDistances.size();
                    std::vector<double>::size_type equal_features_count(0);
                    std::vector<double>::size_type bad_features_count(0);
                    for (std::vector<double>::size_type desc_idx = 0; desc_idx != first.etalonDistances.size(); desc_idx++) {
                        double f = first.etalonDistances[desc_idx];
                        double s = second.etalonDistances[desc_idx];
                        if (f >= s) {
                            bad_features_count++;
                        }
                        if (f == s) {
                            equal_features_count++;
                        }
                    }
                    if (bad_features_count == all_features_count && equal_features_count != all_features_count) {
                        isNotOptimal = true;
                        break;
                    }
                }
            }
            mNext[idx] = isNotOptimal;
            mSurvivors[idx] = !isNotOptimal;
        }
    }
}

PathFinderActivity::AcceptMorphs::AcceptMorphs(
        MoleculeVector &morphs, std::vector<bool> &survivors,
        PathFinderContext &ctx, SmileSet &modifiedParents
        ) :
mMorphs(morphs),
mSurvivors(survivors),
mCtx(ctx),
mModifiedParents(modifiedParents),
mSurvivorCount(0) {
    assert(mMorphs.size() == mSurvivors.size());
}

PathFinderActivity::AcceptMorphs::AcceptMorphs(
        AcceptMorphs &toSplit, tbb::split
        ) :
mCtx(toSplit.mCtx),
mMorphs(toSplit.mMorphs),
mSurvivors(toSplit.mSurvivors),
mModifiedParents(toSplit.mModifiedParents),
mSurvivorCount(0) {
}

void PathFinderActivity::AcceptMorphs::operator()(
        const tbb::blocked_range<size_t> &r, tbb::pre_scan_tag) {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {
        if (mSurvivors[idx]) {
            ++mSurvivorCount;
        }
    }
}

void PathFinderActivity::AcceptMorphs::operator()(
        const tbb::blocked_range<size_t> &r, tbb::final_scan_tag) {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {
        if (mSurvivors[idx]) {
//            if (mSurvivorCount < mCtx.params.cntCandidatesToKeepMax) {
            if (true) {
                PathFinderContext::CandidateMap::accessor ac;

                if (!mCtx.ScaffoldMode()) {
                    mCtx.candidates.insert(ac, mMorphs[idx].smile);
                    ac->second = mMorphs[idx];
                    ac.release();
                } else {
                    PathFinderContext::ScaffoldSmileMap::accessor acScaff;
                    bool success = mCtx.candidateScaffoldMolecules.insert(
                        acScaff, mMorphs[idx].scaffoldSmile);
                    if (!success) {
                        // the scaffold morph is already in candidate tree (it is strange
                        // that it does not happen when scaffold hopping is turned off)
                        continue;
                    }
                    acScaff->second = mMorphs[idx].smile;

                    mCtx.candidates.insert(ac, mMorphs[idx].smile);
                    ac->second = mMorphs[idx];
                    ac.release();
                }

                if (mCtx.candidates.find(ac, mMorphs[idx].parentSmile)) {
                    ac->second.descendants.insert(mMorphs[idx].smile);
                    ac->second.historicDescendants.insert(mMorphs[idx].smile);
                    SmileSet::const_accessor dummy;
                    mModifiedParents.insert(dummy, ac->second.smile);
                } else {
                    assert(false);
                }
            }
            ++mSurvivorCount;
        }
    }
}

void PathFinderActivity::AcceptMorphs::reverse_join(AcceptMorphs &toJoin) {
    mSurvivorCount += toJoin.mSurvivorCount;
}

void PathFinderActivity::AcceptMorphs::assign(AcceptMorphs &toAssign) {
    mSurvivorCount = toAssign.mSurvivorCount;
}

PathFinderActivity::UpdateTree::UpdateTree(PathFinderContext &ctx) :
mCtx(ctx) {
}

void PathFinderActivity::UpdateTree::operator()(
        const SmileSet::range_type &modifiedParents) const {
    PathFinderActivity::SmileSet::iterator itParent;
    for (itParent = modifiedParents.begin();
            itParent != modifiedParents.end(); itParent++) {

        // Determine what child is the closest to the etalon.
        double minDistance = DBL_MAX;
        PathFinderContext::CandidateMap::accessor acParent;
        if (mCtx.candidates.find(acParent, itParent->first)) {

            std::set<std::string>::iterator itChild;
            for (itChild = acParent->second.descendants.begin();
                    itChild != acParent->second.descendants.end();
                    itChild++) {

                PathFinderContext::CandidateMap::const_accessor acChild;
                if (mCtx.candidates.find(acChild, (*itChild))) {
                    if (acChild->second.distToEtalon < minDistance) {
                        minDistance = acChild->second.distToEtalon;
                    }
                } else {
                    assert(false);
                }

            }

        } else {
            assert(false);
        }

        // Update the tree branch towards root.
//        while ((!mCtx.ScaffoldMode() && !acParent->second.parentSmile.empty()) ||
//                (mCtx.ScaffoldMode() && acParent->first.compare(mCtx.tempSource.smile) != 0)) {
        while (true) {
            if (minDistance < acParent->second.distToEtalon) {
                acParent->second.itersWithoutDistImprovement = 0;
            }
            std::string smile = acParent->second.parentSmile;
            if (smile.empty()) {
                acParent->second.itersWithoutDistImprovement = 0;
            }
            acParent.release();
            if (!smile.empty()) {
                mCtx.candidates.find(acParent, smile);
                assert(!acParent.empty());
            } else {
                break;
            }
        }

    }
}

PathFinderActivity::PruneTree::PruneTree(PathFinderContext &ctx, SmileSet &deferred) :
mCtx(ctx),
mDeferred(deferred) {
}

void PathFinderActivity::PruneTree::operator()(
        const std::string &smile, tbb::parallel_do_feeder<std::string> &feeder) const {
    PathFinderContext::CandidateMap::accessor ac;
    mCtx.candidates.find(ac, smile);
    assert(!ac.empty());

    if (ac->second.decayed) {
        std::set<std::string>::const_iterator it;
        for (it = ac->second.descendants.begin();
                it != ac->second.descendants.end(); it++) {
            feeder.add(*it);
        }
        return;
    }

    bool decayed = ac->second.itersFresh > mCtx.params.decayThreshold;
    if (decayed) {
        SynchCout("Decaying " + ac->second.id + "...");
        ac->second.decayed = true;
        std::set<std::string>::const_iterator it;
        for (it = ac->second.descendants.begin();
                it != ac->second.descendants.end(); it++) {
            feeder.add(*it);
        }
        return;
    }

    SmileSet::const_accessor dummy;
    bool deferred = mDeferred.find(dummy, smile);
    bool prune = (deferred ||
            (ac->second.itersWithoutDistImprovement > mCtx.params.itThreshold));
    if (prune && !ac->second.parentSmile.empty()) {
        SynchCout("Pruning " + ac->second.id + "...");

        bool tooManyDerivations = false;
        PathFinderContext::MorphDerivationMap::const_accessor acDerivations;
        if (mCtx.morphDerivations.find(acDerivations, smile)) {
            tooManyDerivations = (acDerivations->second > mCtx.params.cntMaxMorphs);
        }

        bool pruneThis = (deferred || tooManyDerivations);

        if (pruneThis) {
            PathFinderContext::CandidateMap::accessor acParent;
            mCtx.candidates.find(acParent, ac->second.parentSmile);
            assert(!acParent.empty());

            acParent->second.descendants.erase(smile);
                acParent.release();
            ac.release();

            EraseSubTree(smile);
        } else {
                std::set<std::string>::const_iterator it;
                for (it = ac->second.descendants.begin();
                        it != ac->second.descendants.end(); it++) {
                    EraseSubTree(*it);
                }
                ac->second.descendants.clear();
                ac->second.itersWithoutDistImprovement = 0;
            }

    } else {
        std::set<std::string>::const_iterator it;
        for (it = ac->second.descendants.begin();
                it != ac->second.descendants.end(); it++) {
            feeder.add(*it);
        }
    }
}

void PathFinderActivity::PruneTree::EraseSubTree(const std::string &root) const {
    std::deque<std::string> toErase;
    toErase.push_back(root);

    while (!toErase.empty()) {
        std::string current = toErase.front();
        toErase.pop_front();

        PathFinderContext::CandidateMap::accessor ac;
        mCtx.candidates.find(ac, current);
        assert(!ac.empty());

        std::set<std::string>::const_iterator it;
        for (it = ac->second.descendants.begin();
                it != ac->second.descendants.end(); it++) {
            toErase.push_back(*it);
        }

        mCtx.prunedDuringThisIter.push_back(current);
        if (mCtx.ScaffoldMode()) {
            bool success = mCtx.candidateScaffoldMolecules.erase(
                    ac->second.scaffoldSmile);
            assert(success);
        }
        mCtx.candidates.erase(ac);
    }
}

PathFinderActivity::AccumulateTime::AccumulateTime(PathFinderContext &ctx) :
mCtx(ctx) {
    mTimestamp = std::clock();
}

unsigned int PathFinderActivity::AccumulateTime::GetElapsedSeconds(bool reset) {
    clock_t current = std::clock();
    unsigned int seconds =
            (unsigned int) ((current - mTimestamp) / CLOCKS_PER_SEC);
    if (reset) {
        mTimestamp = current;
    }
    return seconds;
}

void PathFinderActivity::AccumulateTime::ReportElapsedMiliseconds(
        const std::string &consumer, bool reset) {
    clock_t current = std::clock();
#if PATHFINDER_REPORTING == 1
    std::ostringstream stream;
    stream << mCtx.jobId << "/" << mCtx.iterIdx << ": " <<
            consumer << " consumed " << current - mTimestamp << " msec.";
    SynchCout(stream.str());
#endif
    if (reset) {
        mTimestamp = current;
    }
}

void PathFinderActivity::AccumulateTime::Reset() {
    mTimestamp = std::clock();
}

/**
 * Accept single morph with given index do not control anything.
 * @param idx Index or morph to accept.
 * @param morphs List of morphs.
 * @param ctx Context.
 * @param modifiedParents Parent to modify.
 *
 * Unused method (scaffold hopping is not implemented here).
 */
void acceptMorph2(
        size_t idx,
        PathFinderActivity::MoleculeVector &morphs,
        PathFinderContext &ctx,
        PathFinderActivity::SmileSet &modifiedParents) {
    PathFinderContext::CandidateMap::accessor ac;
    ctx.candidates.insert(ac, morphs[idx].smile);
    ac->second = morphs[idx];
    ac.release();

    if (ctx.candidates.find(ac, morphs[idx].parentSmile)) {
        ac->second.descendants.insert(morphs[idx].smile);
        ac->second.historicDescendants.insert(morphs[idx].smile);
        PathFinderActivity::SmileSet::const_accessor dummy;
        modifiedParents.insert(dummy, ac->second.smile);
    } else {
        assert(false);
    }
}

/**
 * Accept morphs from list. If there is no decoy the PathFinder::AcceptMorphs is
 * used. Otherwise for each decoy the same number of best candidates is accepted.
 * @param morphs Candidates.
 * @param survivors Survive index.
 * @param ctx Context.
 * @param modifiedParents
 * @param decoySize Number of decoy used during exploration.
 */
void acceptMorphs2(PathFinderActivity::MoleculeVector &morphs,
        std::vector<bool> &survivors,
        PathFinderContext &ctx,
        PathFinderActivity::SmileSet &modifiedParents,
        int decoySize) {
    PathFinderActivity::AcceptMorphs acceptMorphs(morphs, survivors, ctx, modifiedParents);
    // FIXME
    // Current TBB version does not support parallel_scan cancellation.
    // If it will be improved in the future, pass task_group_context
    // argument similarly as in parallel_for.
    tbb::parallel_scan(
            tbb::blocked_range<size_t>(0, morphs.size()),
            acceptMorphs, tbb::auto_partitioner());
    SynchCout("\tAcceptance ratio (iteration #"
            + NumberToString(ctx.iterIdx) + "): "
            + NumberToString(acceptMorphs.mSurvivorCount) + "/"
            + NumberToString(morphs.size()) + ".");
    return;
}

std::pair<double, double> PathFinderActivity::SaveIterationData::getClosestTestActives(
    MolpherMolecule &inputMol
    , PathFinderContext::CandidateMap& probedMols
    , PathFinderContext& ctx
    , MolpherMolecule*& testActiveStruct
    , MolpherMolecule*& testActiveActivity
) {
    SimCoefCalculator scCalc(ctx.simCoeffSelector, ctx.fingerprintSelector);
    double current_min_struct = DBL_MAX;
    double current_min_activity = DBL_MAX;
    for (
            PathFinderContext::CandidateMap::iterator testIt = probedMols.begin();
            testIt != probedMols.end(); testIt++
            ) {
        assert(inputMol.id.compare(testIt->second.id) != 0);
        RDKit::RWMol *mol = NULL;
        RDKit::RWMol *test = NULL;
        try {
            mol = RDKit::SmilesToMol(inputMol.smile);
            test = RDKit::SmilesToMol(testIt->second.smile);
            if (mol && test) {
                RDKit::MolOps::Kekulize(*mol);
                RDKit::MolOps::Kekulize(*test);
            } else {
                throw ValueErrorException("");
            }
        } catch (const ValueErrorException &exc) {
            delete mol;
            delete test;
            continue;
        }

        double simCoeff = scCalc.GetSimCoef(mol, test);
        double struc_dist = scCalc.ConvertToDistance(simCoeff);
        if (struc_dist < current_min_struct) {
            current_min_struct = struc_dist;
            testActiveStruct = &(testIt->second);
        }

        MolpherMolecule& testMol = testIt->second;
        double activity_dist = inputMol.GetDistanceFrom(testMol, ctx.descWeights);
        if (activity_dist < current_min_activity) {
            current_min_activity = activity_dist;
            testActiveActivity = &(testIt->second);
        }

        delete mol;
        delete test;
    }
    return std::make_pair(current_min_struct, current_min_activity);
}

void PathFinderActivity::SaveIterationData::saveCSVData(
    MolpherMolecule& mol
    , PathFinderContext::CandidateMap& probedMols
    , PathFinderContext& ctx
    , CSVparse::CSV& morphingData
) {
    std::vector<std::string> stringData;
    std::vector<double> floatData;
    stringData.push_back("NA");
    floatData.push_back(NAN);

    MolpherMolecule* closestTestActiveStruct = NULL;
    MolpherMolecule* closestTestActiveActivity = NULL;
    std::pair<double, double> distStructActivity;
    distStructActivity = getClosestTestActives(mol, probedMols, ctx, closestTestActiveStruct, closestTestActiveActivity);

    // closest in strctural space
    if (closestTestActiveStruct) {
        floatData[0] = distStructActivity.first;
        stringData[0] = closestTestActiveStruct->id;
    } else {
        stringData[0] = "NA";
        floatData[0] = NAN;
    }
    morphingData.addStringData("ClosestStructID", stringData);
    morphingData.addFloatData("ClosestStructDistance", floatData);

    // closest in activity space
    if (closestTestActiveActivity) {
        floatData[0] = distStructActivity.second;
        stringData[0] = closestTestActiveActivity->id;
    } else {
        stringData[0] = "NA";
        floatData[0] = NAN;
    }
    morphingData.addStringData("ClosestActivityID", stringData);
    morphingData.addFloatData("ClosestActivityDistance", floatData);
}

void PathFinderActivity::operator()() {
    SynchCout(std::string("PathFinder thread started."));

    tbb::task_scheduler_init scheduler;
    if (mThreadCnt > 0) {
        scheduler.terminate();
        scheduler.initialize(mThreadCnt);
    }

    bool canContinueCurrentJob = false;
    bool pathFound = false;
    std::vector<std::string> startMols;
    CSVparse::CSV morphingData;

    while (true) {

        if (!canContinueCurrentJob) {
            if (mJobManager->GetJob(mCtx)) {
                canContinueCurrentJob = true;
                pathFound = false;

                // Initialize the first iteration of a job.
                if (mCtx.candidates.empty()) {
                    assert(mCtx.iterIdx == 0);
                    assert(mCtx.candidateScaffoldMolecules.empty());

                    if (mCtx.params.startMolMaxCount == 0) {
                        mCtx.params.startMolMaxCount = mCtx.sourceMols.size();
                    }

                    PathFinderContext::CandidateMap::accessor ac;
                    if (!mCtx.ScaffoldMode()) {
                        int counter = 1;
                        for (PathFinderContext::CandidateMap::iterator it = mCtx.sourceMols.begin(); it != mCtx.sourceMols.end(); it++) {
                            mCtx.candidates.insert(ac, it->first);
                            startMols.push_back(it->first);
                            //SynchCout(it->first);
                            ac->second = it->second;

                            // @TODO Save candidate molecules (ID, SMILES, EthalonDistance, Parent, ...)

                            ++counter;
                            if (counter > mCtx.params.startMolMaxCount) {
                                break;
                            }
                        }

                    } else {
                        assert(mCtx.scaffoldSelector == SF_MOST_GENERAL);

                        Scaffold *scaff = ScaffoldDatabase::Get(mCtx.scaffoldSelector);

                        std::string scaffSource;
                        scaff->GetScaffold(mCtx.source.smile, &scaffSource);
                        mCtx.tempSource.scaffoldSmile = scaffSource;
                        std::string scaffTarget;
                        scaff->GetScaffold(mCtx.target.smile, &scaffTarget);
                        mCtx.target.scaffoldSmile = scaffTarget;

                        mCtx.candidates.insert(ac, mCtx.tempSource.smile);
                        ac->second = mCtx.tempSource;

                        PathFinderContext::ScaffoldSmileMap::accessor acScaff;
                        mCtx.candidateScaffoldMolecules.insert(acScaff, scaffSource);
                        acScaff->second = mCtx.source.smile;
                        acScaff.release();

                        mCtx.pathScaffoldMolecules.insert(acScaff, scaffSource);
                        acScaff->second = mCtx.source.smile;
                        acScaff.release();
                        mCtx.pathScaffoldMolecules.insert(acScaff, scaffTarget);
                        acScaff->second = mCtx.target.smile;

                        std::string scaffDecoy;
                        std::vector<MolpherMolecule>::iterator it;
                        for (it = mCtx.decoys.begin(); it != mCtx.decoys.end(); ++it) {
                            scaff->GetScaffold(it->smile, &scaffDecoy);
                            it->scaffoldSmile = scaffDecoy;
                            it->scaffoldLevelCreation = mCtx.scaffoldSelector;
                        }

                        delete scaff;
                    }
                }
            } else {
                break; // Thread termination.
            }
        }

        try {

            if (!Cancelled()) {
                mJobManager->GetFingerprintSelector(mCtx.fingerprintSelector);
                mJobManager->GetSimCoeffSelector(mCtx.simCoeffSelector);
                mJobManager->GetDimRedSelector(mCtx.dimRedSelector);
                mJobManager->GetChemOperSelectors(mCtx.chemOperSelectors);
                mJobManager->GetParams(mCtx.params);
                mJobManager->GetDecoys(mCtx.decoys);
                mCtx.prunedDuringThisIter.clear();
            }

            AccumulateTime molpherStopwatch(mCtx);
            AccumulateTime stageStopwatch(mCtx);

            SynchCout("Starting with " + NumberToString(mCtx.candidates.size()) + " source molecules...");
            MoleculeVector currentBag;
            FindNextBag findBag(currentBag);
            if (!Cancelled()) {
                tbb::parallel_for(
                        PathFinderContext::CandidateMap::range_type(mCtx.candidates),
                        findBag, tbb::auto_partitioner(), *mTbbCtx);
                stageStopwatch.ReportElapsedMiliseconds("FindNextBag", true);
            }

            MoleculeVector morphs;
            CollectMorphs collectMorphs(morphs);
            Scaffold *scaff = mCtx.ScaffoldMode() ?
                    ScaffoldDatabase::Get(mCtx.scaffoldSelector) : NULL;
            std::vector<ChemOperSelector> chemOperSelectors =
                    !mCtx.ScaffoldMode() || mCtx.scaffoldSelector == SF_ORIGINAL_MOLECULE ?
                        mCtx.chemOperSelectors : scaff->GetUsefulOperators();
            for (MoleculeVector::iterator it = currentBag.begin(); it != currentBag.end(); it++) {
                MolpherMolecule &candidate = (*it);
                unsigned int morphAttempts = mCtx.params.cntMorphs;
                if (!Cancelled()) {
                    morphs.reserve(morphs.size() + morphAttempts);

                    GenerateMorphs(
                            candidate,
                            morphAttempts,
                            chemOperSelectors,
                            mCtx.decoys,
                            *mTbbCtx,
                            &collectMorphs,
                            MorphCollector2,
                            scaff);
                    PathFinderContext::MorphDerivationMap::accessor ac;

                    if (mCtx.morphDerivations.find(ac, candidate.smile)) {
                        ac->second += collectMorphs.WithdrawCollectAttemptCount();
                    } else {
                        mCtx.morphDerivations.insert(ac, candidate.smile);
                        ac->second = collectMorphs.WithdrawCollectAttemptCount();
                    }
                }

                if (Cancelled()) {
                    break;
                }
            }
            delete scaff;
            morphs.shrink_to_fit();

            if (!Cancelled()) {
                std::stringstream ss;
                ss << "\tmorphs count: " << morphs.size();
                SynchCout(ss.str());
                stageStopwatch.ReportElapsedMiliseconds("GenerateMorphs", true);
            }

            std::vector<bool> survivors;
            survivors.resize(morphs.size(), true);
            FilterMorphs filterMorphs(mCtx, morphs.size(), morphs, survivors);
            if (!Cancelled()) {
                if (mCtx.params.useSyntetizedFeasibility) {
                    SynchCout("\tUsing syntetize feasibility");
                }
                tbb::parallel_for(
                        tbb::blocked_range<size_t>(0, morphs.size()),
                        filterMorphs, tbb::auto_partitioner(), *mTbbCtx);
                stageStopwatch.ReportElapsedMiliseconds("FilterMorphs", true);
            }

            // prepare directory for descriptor computation
            std::string output_dir(mJobManager->GetStorageDir());
            std::string storage_dir(GenerateDirname(output_dir, mCtx.jobId,
                    "_" + NumberToString(mCtx.iterIdx)));
            try {
                boost::filesystem::create_directories(storage_dir);
            } catch (boost::filesystem::filesystem_error &exc) {
                SynchCout(exc.what());
            }

            // calculate descriptors
            // TODO: could be concurrent (use the tbb::concurrent_* structures in PathFinderContext) -> concurrent file IO needed too
            if (!Cancelled()) {

                unsigned int mols_per_step = mCtx.padelBatchSize;
                unsigned int morph_count = morphs.size();
                unsigned int steps = morph_count / mols_per_step + 1;

                for (unsigned int i = 0; i != steps; i++) {

                    std::string storage_path(GenerateDirname(output_dir, mCtx.jobId,
                            "_" + NumberToString(mCtx.iterIdx) + "/run_" + NumberToString(i)));

                    std::shared_ptr<DescriptorSource> calculator;
                    calculator = DescriptorSource::createPaDEL(
                            mCtx.padelPath, storage_path
                            , mCtx.relevantDescriptorNames, mThreadCnt,
                            "/descriptors.csv");

                    bool mol_added = false;
                    for (unsigned int idx = i * mols_per_step; idx != i * mols_per_step + mols_per_step; idx++) {
                        if (idx == morph_count) {
                            break;
                        }
                        MolpherMolecule &morph = morphs[idx];
                        morph.id = "MORPH_" + NumberToString(mCtx.iterIdx) + "_" + NumberToString(idx + 1);
                        if (survivors[idx]) {
                            mol_added = true;
                            calculator->add(morph);
                        }
                    }

                    // if there are no survivors, just continue
                    if (!mol_added) {
                        continue;
                    }

                    try {
                        boost::filesystem::create_directories(storage_path);
                    } catch (boost::filesystem::filesystem_error &exc) {
                        SynchCout(exc.what());
                    }

                    // compute descriptors using PaDEL
                    calculator->compute();

                    // load and normalize the data + compute etalon distances
                    for (unsigned int idx = i * mols_per_step; (idx != i * mols_per_step + mols_per_step) ; idx++) {
                        if (idx == morph_count) {
                            break;
                        }
                        if (survivors[idx]) {
                            MolpherMolecule &morph = morphs[idx];
                            morph.SaveDescriptors(calculator->get(morph), mCtx.relevantDescriptorNames);
                            morph.normalizeDescriptors(mCtx.normalizationCoefficients, mCtx.imputedValues);
                            morph.ComputeEtalonDistances(mCtx.etalonValues, mCtx.descWeights);
                        }
                    }
                }

                if (!Cancelled()) {
                    stageStopwatch.ReportElapsedMiliseconds("ComputeDescriptors", true);
                }
            }

            // MOOP
            std::vector<bool> next = survivors; // morphs scheduled for next MOOP run
            MOOPFilter MOOPfiltering(morphs, survivors, next);
            if (!Cancelled()) {
                unsigned int counter = 0;
                while (mCtx.params.maxMOOPruns > counter) {
                    unsigned int next_c = 0;
                    unsigned int accepted_c = 0;
                    for (unsigned int idx = 0; idx != next.size(); idx++) {
                        if (next[idx]) ++next_c;
                        if (survivors[idx]) ++accepted_c;
                    }
                    SynchCout("\tNext MOOP run (#" + NumberToString(counter + 1) + ") input: " + NumberToString(next_c));
                    SynchCout("\tSurvivors overall: " + NumberToString(accepted_c));
                    if (next_c == 0) break;
                    tbb::parallel_for(
                        tbb::blocked_range<size_t>(0, morphs.size()),
                        MOOPfiltering, tbb::auto_partitioner(), *mTbbCtx);
                    ++counter;
                }
                unsigned int next_c = 0;
                unsigned int accepted_c = 0;
                for (unsigned int idx = 0; idx != next.size(); idx++) {
                    if (next[idx]) ++next_c;
                    if (survivors[idx]) ++accepted_c;
                }
                SynchCout("\tLast MOOP run (#" + NumberToString(counter) + ") non-optimals: " + NumberToString(next_c));
                SynchCout("\tSurvivors overall: " + NumberToString(accepted_c));
                stageStopwatch.ReportElapsedMiliseconds("MOOPfiltering", true);
            }

            // @TODO Save morph_it as CSV file.

            // Now we need to accept morphs ie. move the lucky one from
            // morphs -> survivors
            SmileSet modifiedParents;
            acceptMorphs2(morphs, survivors, mCtx, modifiedParents, mCtx.decoys.size());
            stageStopwatch.ReportElapsedMiliseconds("AcceptMorphs", true);

            UpdateTree updateTree(mCtx);
            if (!Cancelled()) {
                tbb::parallel_for(SmileSet::range_type(modifiedParents),
                        updateTree, tbb::auto_partitioner(), *mTbbCtx);
                stageStopwatch.ReportElapsedMiliseconds("UpdateTree", true);
            }

            SmileSet deferredSmiles;
            PruneTree pruneTree(mCtx, deferredSmiles);
            if (!Cancelled()) {
                SmileVector pruningQueue;
                for (std::vector<std::string>::iterator sourceIt = startMols.begin();
                        sourceIt != startMols.end(); sourceIt++) {
                    pruningQueue.push_back(*sourceIt);
                }
                tbb::parallel_do(
                            pruningQueue.begin(), pruningQueue.end(), pruneTree, *mTbbCtx);
                    assert(!mCtx.ScaffoldMode() ?
                            true :
                            mCtx.candidates.size() == mCtx.candidateScaffoldMolecules.size());
                stageStopwatch.ReportElapsedMiliseconds("PruneTree", true);
            }

            double distance = DBL_MAX;
            for (auto itCandidates = mCtx.candidates.begin();
                    itCandidates != mCtx.candidates.end(); ++itCandidates) {
                if (itCandidates->second.distToEtalon < distance) {
                    distance = itCandidates->second.distToEtalon;
                }
            }
            std::stringstream ss;
            ss << mCtx.jobId << "/" << mCtx.iterIdx << ": "
                    << "The min. distance to etalon: " << distance;
            SynchCout(ss.str());

            // @TODO Save candidates list.

            if (!Cancelled()) {
                mCtx.iterIdx += 1;
                mCtx.elapsedSeconds += molpherStopwatch.GetElapsedSeconds();

                if (canContinueCurrentJob) {
                    bool itersDepleted = (mCtx.params.cntIterations <= mCtx.iterIdx);
                    bool timeDepleted = (mCtx.params.timeMaxSeconds <= mCtx.elapsedSeconds);
                    canContinueCurrentJob = (!itersDepleted && !timeDepleted);

                    if (itersDepleted) {
                        std::stringstream ss;
                        ss << mCtx.jobId << "/" << mCtx.iterIdx << ": "
                                << "The max number of iterations has been reached.";
                        SynchCout(ss.str());
                    }
                    if (timeDepleted) {
                        std::stringstream ss;
                        ss << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": "
                                << "We ran out of time.";
                        SynchCout(ss.str());
                    }
                    if (!canContinueCurrentJob) {
                        IterationSnapshot snp;
                        PathFinderContext::ContextToSnapshot(mCtx, snp);
                    }
                }
            }

        } catch (tbb::tbb_exception &exc) {
            SynchCout(std::string(exc.what()));
            canContinueCurrentJob = false;
        }

        canContinueCurrentJob = mJobManager->CommitIteration(
                mCtx, canContinueCurrentJob, pathFound);

    }

    SynchCout(std::string("PathFinder thread terminated."));
}

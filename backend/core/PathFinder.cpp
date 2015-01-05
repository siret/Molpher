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

#include <cassert>
#include <cmath>
#include <cfloat>
#include <string>
#include <sstream>

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
#include "JobManager.h"
#include "PathFinder.h"

PathFinder::PathFinder(
        tbb::task_group_context *tbbCtx, JobManager *jobManager, int threadCnt
        ) :
mTbbCtx(tbbCtx),
mJobManager(jobManager),
mThreadCnt(threadCnt) {
}

PathFinder::~PathFinder() {
}

bool PathFinder::Cancelled() {
    return mTbbCtx->is_group_execution_cancelled();
}

PathFinder::FindLeaves::FindLeaves(MoleculeVector &leaves) :
mLeaves(leaves) {
}

void PathFinder::FindLeaves::operator()(
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

PathFinder::CollectMorphs::CollectMorphs(MoleculeVector &morphs) :
mMorphs(morphs) {
    mCollectAttemptCount = 0;
}

void PathFinder::CollectMorphs::operator()(const MolpherMolecule &morph) {
    ++mCollectAttemptCount; // atomic
    SmileSet::const_accessor dummy;
    if (mDuplicateChecker.insert(dummy, morph.smile)) {
        mMorphs.push_back(morph);
    } else {
        // ignore duplicate
    }
}

unsigned int PathFinder::CollectMorphs::WithdrawCollectAttemptCount() {
    unsigned int ret = mCollectAttemptCount;
    mCollectAttemptCount = 0;
    return ret;
}

void MorphCollector(MolpherMolecule *morph, void *functor) {
    PathFinder::CollectMorphs *collect =
            (PathFinder::CollectMorphs *) functor;
    (*collect)(*morph);
}

// return true if "a" is closes to target then "b"

bool PathFinder::CompareMorphs::operator()(
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

PathFinder::FilterMorphs::FilterMorphs(PathFinderContext &ctx,
        size_t globalMorphCount, MoleculeVector &morphs, std::vector<bool> &survivors
        ) :
mCtx(ctx),
mGlobalMorphCount(globalMorphCount),
mMorphs(morphs),
mSurvivors(survivors) {
    assert(mMorphs.size() == mSurvivors.size());
}

void PathFinder::FilterMorphs::operator()(const tbb::blocked_range<size_t> &r) const {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {

        double acceptProbability = 1.0;
        bool isTarget = !mCtx.ScaffoldMode() ?
                (mMorphs[idx].smile.compare(mCtx.target.smile) == 0) :
                (mMorphs[idx].scaffoldSmile.compare(mCtx.target.scaffoldSmile) == 0);
        if (idx >= mCtx.params.cntCandidatesToKeep && !isTarget) {
            acceptProbability =
                    0.25 - (idx - mCtx.params.cntCandidatesToKeep) /
                    ((mGlobalMorphCount - mCtx.params.cntCandidatesToKeep) * 4.0);
        }

        bool mightSurvive =
                SynchRand::GetRandomNumber(0, 99) < (int) (acceptProbability * 100);
        if (mightSurvive) {
            bool isDead = false;
            bool badWeight = false;
            bool badSascore = false;
            bool alreadyExists = false;
            bool alreadyTriedByParent = false;
            bool tooManyProducedMorphs = false;

            // Tests are ordered according to their cost.
            // Added test for SAScore

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs);
            if (!isDead) {
                badWeight =
                        (mMorphs[idx].molecularWeight <
                        mCtx.params.minAcceptableMolecularWeight) ||
                        (mMorphs[idx].molecularWeight >
                        mCtx.params.maxAcceptableMolecularWeight);
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs);

            if (!isDead) {
                if (mCtx.params.useSyntetizedFeasibility) {
                    badSascore = mMorphs[idx].sascore > 6.0; // questionable, it is recommended value from Ertl
                    // in case of badSascore print message
                    if (badSascore) {
                        //std::stringstream ss;
                        //ss << "bad sasscore: " << mMorphs[idx].smile << " : " << mMorphs[idx].sascore;
                        //SynchCout( ss.str() );
                    }
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs);
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
                    alreadyTriedByParent || tooManyProducedMorphs);
            if (!isDead) {
                PathFinderContext::CandidateMap::const_accessor ac;
                if (mCtx.candidates.find(ac, mMorphs[idx].parentSmile)) {
                    alreadyTriedByParent = (
                            ac->second.historicDescendants.find(mMorphs[idx].smile)
                            !=
                            ac->second.historicDescendants.end());
                } else {
                    assert(false);
                }
            }
            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs);
            if (!isDead) {
                PathFinderContext::MorphDerivationMap::const_accessor ac;
                if (mCtx.morphDerivations.find(ac, mMorphs[idx].smile)) {
                    tooManyProducedMorphs =
                            (ac->second > mCtx.params.cntMaxMorphs);
                }
            }

            isDead = (badWeight || badSascore || alreadyExists ||
                    alreadyTriedByParent || tooManyProducedMorphs);
            mSurvivors[idx] = !isDead;
        }
    }

}

PathFinder::AcceptMorphs::AcceptMorphs(
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

PathFinder::AcceptMorphs::AcceptMorphs(
        AcceptMorphs &toSplit, tbb::split
        ) :
mCtx(toSplit.mCtx),
mMorphs(toSplit.mMorphs),
mSurvivors(toSplit.mSurvivors),
mModifiedParents(toSplit.mModifiedParents),
mSurvivorCount(0) {
}

void PathFinder::AcceptMorphs::operator()(
        const tbb::blocked_range<size_t> &r, tbb::pre_scan_tag) {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {
        if (mSurvivors[idx]) {
            ++mSurvivorCount;
        }
    }
}

void PathFinder::AcceptMorphs::operator()(
        const tbb::blocked_range<size_t> &r, tbb::final_scan_tag) {
    for (size_t idx = r.begin(); idx != r.end(); ++idx) {
        if (mSurvivors[idx]) {
            if (mSurvivorCount < mCtx.params.cntCandidatesToKeepMax) {
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

void PathFinder::AcceptMorphs::reverse_join(AcceptMorphs &toJoin) {
    mSurvivorCount += toJoin.mSurvivorCount;
}

void PathFinder::AcceptMorphs::assign(AcceptMorphs &toAssign) {
    mSurvivorCount = toAssign.mSurvivorCount;
}

PathFinder::UpdateTree::UpdateTree(PathFinderContext &ctx) :
mCtx(ctx) {
}

void PathFinder::UpdateTree::operator()(
        const SmileSet::range_type &modifiedParents) const {
    PathFinder::SmileSet::iterator itParent;
    for (itParent = modifiedParents.begin();
            itParent != modifiedParents.end(); itParent++) {

        // Determine what child is the closest to the target.
        double minDistance = DBL_MAX;
        PathFinderContext::CandidateMap::accessor acParent;
        if (mCtx.candidates.find(acParent, itParent->first)) {

            std::set<std::string>::iterator itChild;
            for (itChild = acParent->second.descendants.begin();
                    itChild != acParent->second.descendants.end();
                    itChild++) {

                PathFinderContext::CandidateMap::const_accessor acChild;
                if (mCtx.candidates.find(acChild, (*itChild))) {
                    if (acChild->second.distToTarget < minDistance) {
                        minDistance = acChild->second.distToTarget;
                    }
                } else {
                    assert(false);
                }

            }

        } else {
            assert(false);
        }

        // Update the tree branch towards root.
        while ((!mCtx.ScaffoldMode() && !acParent->second.parentSmile.empty()) ||
                (mCtx.ScaffoldMode() && acParent->first.compare(mCtx.tempSource.smile) != 0)) {
            if (minDistance < acParent->second.distToTarget) {
                acParent->second.itersWithoutDistImprovement = 0;
            }
            std::string smile = acParent->second.parentSmile;
            acParent.release();
            mCtx.candidates.find(acParent, smile);
            assert(!acParent.empty());
        }

    }
}

PathFinder::PruneTree::PruneTree(PathFinderContext &ctx, SmileSet &deferred) :
mCtx(ctx),
mDeferred(deferred) {
}

void PathFinder::PruneTree::operator()(
        const std::string &smile, tbb::parallel_do_feeder<std::string> &feeder) const {
    PathFinderContext::CandidateMap::accessor ac;
    mCtx.candidates.find(ac, smile);
    assert(!ac.empty());

    SmileSet::const_accessor dummy;
    bool deferred = mDeferred.find(dummy, smile);
    bool prune = (deferred ||
            (ac->second.itersWithoutDistImprovement > mCtx.params.itThreshold));
    if (prune) {

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

void PathFinder::PruneTree::EraseSubTree(const std::string &root) const {
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

PathFinder::AccumulateTime::AccumulateTime(PathFinderContext &ctx) :
mCtx(ctx) {
    mTimestamp = std::clock();
}

unsigned int PathFinder::AccumulateTime::GetElapsedSeconds(bool reset) {
    clock_t current = std::clock();
    unsigned int seconds =
            (unsigned int) ((current - mTimestamp) / CLOCKS_PER_SEC);
    if (reset) {
        mTimestamp = current;
    }
    return seconds;
}

void PathFinder::AccumulateTime::ReportElapsedMiliseconds(
        const std::string &consumer, bool reset) {
    clock_t current = std::clock();
#if PATHFINDER_REPORTING == 1
    std::ostringstream stream;
    stream << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": " <<
            consumer << " consumed " << current - mTimestamp << " msec.";
    SynchCout(stream.str());
#endif
    if (reset) {
        mTimestamp = current;
    }
}

void PathFinder::AccumulateTime::Reset() {
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
void acceptMorph(
        size_t idx,
        PathFinder::MoleculeVector &morphs,
        PathFinderContext &ctx,
        PathFinder::SmileSet &modifiedParents) {
    PathFinderContext::CandidateMap::accessor ac;
    ctx.candidates.insert(ac, morphs[idx].smile);
    ac->second = morphs[idx];
    ac.release();

    if (ctx.candidates.find(ac, morphs[idx].parentSmile)) {
        ac->second.descendants.insert(morphs[idx].smile);
        ac->second.historicDescendants.insert(morphs[idx].smile);
        PathFinder::SmileSet::const_accessor dummy;
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
void acceptMorphs(PathFinder::MoleculeVector &morphs,
        std::vector<bool> &survivors,
        PathFinderContext &ctx,
        PathFinder::SmileSet &modifiedParents,
        int decoySize) {
    PathFinder::AcceptMorphs acceptMorphs(morphs, survivors, ctx, modifiedParents);
    // FIXME
    // Current TBB version does not support parallel_scan cancellation.
    // If it will be improved in the future, pass task_group_context
    // argument similarly as in parallel_for.
    tbb::parallel_scan(
            tbb::blocked_range<size_t>(0, morphs.size()),
            acceptMorphs, tbb::auto_partitioner());
    return;
}

void PathFinder::operator()() {
    SynchCout(std::string("PathFinder thread started."));

    tbb::task_scheduler_init scheduler;
    if (mThreadCnt > 0) {
        scheduler.terminate();
        scheduler.initialize(mThreadCnt);
    }

    bool canContinueCurrentJob = false;
    bool pathFound = false;

    while (true) {

        if (!canContinueCurrentJob) {
            if (mJobManager->GetJob(mCtx)) {
                canContinueCurrentJob = true;
                pathFound = false;

                // Initialize the first iteration of a job.
                if (mCtx.candidates.empty()) {
                    assert(mCtx.iterIdx == 0);
                    assert(mCtx.candidateScaffoldMolecules.empty());

                    PathFinderContext::CandidateMap::accessor ac;
                    if (!mCtx.ScaffoldMode()) {
                        mCtx.candidates.insert(ac, mCtx.source.smile);
                        ac->second = mCtx.source;
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

            MoleculeVector leaves;
            FindLeaves findLeaves(leaves);
            if (!Cancelled()) {
                tbb::parallel_for(
                        PathFinderContext::CandidateMap::range_type(mCtx.candidates),
                        findLeaves, tbb::auto_partitioner(), *mTbbCtx);
                stageStopwatch.ReportElapsedMiliseconds("FindLeaves", true);
            }

            /* TODO MPI
             MASTER
             prepare light snapshot (PathFinderContext::ContextToLightSnapshot)
             broadcast light snapshot
             convert leaves to std::vector
             scatter leaves over cluster
             convert node-specific part back to MoleculeVector

             SLAVE
             broadcast light snapshot
             convert light snapshot into context (PathFinderContext::SnapshotToContext)
             scatter leaves over cluster
             convert node-specific part back to MoleculeVector
             */

            MoleculeVector morphs;
            CollectMorphs collectMorphs(morphs);
            Scaffold *scaff = mCtx.ScaffoldMode() ?
                    ScaffoldDatabase::Get(mCtx.scaffoldSelector) : NULL;
            std::vector<ChemOperSelector> chemOperSelectors =
                    !mCtx.ScaffoldMode() || mCtx.scaffoldSelector == SF_ORIGINAL_MOLECULE ?
                        mCtx.chemOperSelectors : scaff->GetUsefulOperators();
            for (MoleculeVector::iterator it = leaves.begin(); it != leaves.end(); it++) {
                MolpherMolecule &candidate = (*it);
                unsigned int morphAttempts = mCtx.params.cntMorphs;
                if (candidate.distToTarget < mCtx.params.distToTargetDepthSwitch) {
                    morphAttempts = mCtx.params.cntMorphsInDepth;
                }

                if (!Cancelled()) {
                    morphs.reserve(morphs.size() + morphAttempts);

                    GenerateMorphs(
                            candidate,
                            morphAttempts,
                            mCtx.fingerprintSelector,
                            mCtx.simCoeffSelector,
                            chemOperSelectors,
                            mCtx.target,
                            mCtx.decoys,
                            *mTbbCtx,
                            &collectMorphs,
                            MorphCollector,
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
                stageStopwatch.ReportElapsedMiliseconds("GenerateMorphs", true);
            }

            CompareMorphs compareMorphs;
            if (!Cancelled()) {
                /* FIXME
                 Current TBB version does not support parallel_sort cancellation.
                 If it will be improved in the future, pass task_group_context
                 argument similarly as in parallel_for. */
                tbb::parallel_sort(
                        morphs.begin(), morphs.end(), compareMorphs);
                stageStopwatch.ReportElapsedMiliseconds("SortMorphs", true);
            }

            /* TODO MPI
             MASTER
             gather snapshots from slaves
             update mCtx.morphDerivations according to gathered snapshots (consider using TBB for this)
             convert morphs to std::vector
             gather other morphs from slaves
               - each vector is pre-sorted and without duplicates
             integrate all morph vectors into final vector (consider using TBB for this)
               - check for cross-vector duplicates
               - merge sort

             SLAVE
             convert context to full snapshot (PathFinderContext::ContextToSnapshot)
             gather snapshot to master
             convert morphs to std::vector
             gather morphs back to master
             */

            /* TODO MPI
             MASTER
             prepare full snapshot (PathFinderContext::ContextToSnapshot)
             broadcast snapshot
             broadcast morph vector complete size
             scatter morph vector over cluster
             convert node-specific part back to MoleculeVector

             SLAVE
             broadcast snapshot
             convert snapshot into context (PathFinderContext::SnapshotToContext)
             broadcast morph vector complete size
             scatter morph vector over cluster
             convert node-specific part back to MoleculeVector
             */

            std::vector<bool> survivors;
            survivors.resize(morphs.size(), false);
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

            /* TODO MPI
             MASTER
             gather survivors vector from slaves

             SLAVE
             gather survivors vector back to master
             */

            // Now we need to accept morphs ie. move the lucky one from
            // morphs -> survivors
            SmileSet modifiedParents;
            acceptMorphs(morphs, survivors, mCtx, modifiedParents, mCtx.decoys.size());
            stageStopwatch.ReportElapsedMiliseconds("AcceptMorphs", true);

            UpdateTree updateTree(mCtx);
            if (!Cancelled()) {
                tbb::parallel_for(SmileSet::range_type(modifiedParents),
                        updateTree, tbb::auto_partitioner(), *mTbbCtx);
                stageStopwatch.ReportElapsedMiliseconds("UpdateTree", true);
            }

            if (!Cancelled()) {
                if (!mCtx.ScaffoldMode()) {
                    PathFinderContext::CandidateMap::const_accessor acTarget;
                    mCtx.candidates.find(acTarget, mCtx.target.smile);
                    pathFound = !acTarget.empty();
                } else {
                    PathFinderContext::ScaffoldSmileMap::const_accessor acTarget;
                    mCtx.candidateScaffoldMolecules.find(acTarget, mCtx.target.scaffoldSmile);
                    pathFound = !acTarget.empty();
                }
                if (pathFound) {
                    std::stringstream ss;
                    ss << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": ";
                    !mCtx.ScaffoldMode() ?
                            ss << "- - - Path has been found - - -" :
                            ss << "- - - Subpath has been found - - -";
                    SynchCout(ss.str());
                }
            }

            SmileSet deferredSmiles;
            SmileVector pruningQueue;
            PruneTree pruneTree(mCtx, deferredSmiles);
            if (!pathFound && !Cancelled()) {
                // Prepare deferred visual pruning.
                std::vector<MolpherMolecule> deferredMols;
                mJobManager->GetPruned(deferredMols);
                std::vector<MolpherMolecule>::iterator it;
                for (it = deferredMols.begin(); it != deferredMols.end(); it++) {
                    SmileSet::const_accessor dummy;
                    if (it->smile == mCtx.source.smile ||
                            (mCtx.ScaffoldMode() && it->smile == mCtx.tempSource.smile)) {
                        continue;
                    }
                    deferredSmiles.insert(dummy, it->smile);
                }
                deferredMols.clear();

                pruningQueue.push_back(!mCtx.ScaffoldMode() ?
                        mCtx.source.smile : mCtx.tempSource.smile);
                tbb::parallel_do(
                        pruningQueue.begin(), pruningQueue.end(), pruneTree, *mTbbCtx);
                assert(!mCtx.ScaffoldMode() ?
                        true :
                        mCtx.candidates.size() == mCtx.candidateScaffoldMolecules.size());
                stageStopwatch.ReportElapsedMiliseconds("PruneTree", true);
            }

            // calculation of dimension reduction
            if (!Cancelled() && mCtx.params.useVisualisation) {
                DimensionReducer::MolPtrVector molsToReduce;
                int numberOfMolsToReduce = 0;
                if (!mCtx.ScaffoldMode()) {
                    numberOfMolsToReduce = mCtx.candidates.size() +
                            mCtx.decoys.size() + 2;
                } else {
                    numberOfMolsToReduce = mCtx.candidates.size() +
                            mCtx.decoys.size() + mCtx.pathMolecules.size() + 3;
                }
                molsToReduce.reserve(numberOfMolsToReduce);
                // add all candidate
                PathFinderContext::CandidateMap::iterator itCandidates;
                for (itCandidates = mCtx.candidates.begin();
                        itCandidates != mCtx.candidates.end(); itCandidates++) {
                    molsToReduce.push_back(&itCandidates->second);
                }
                // add all decoys
                std::vector<MolpherMolecule>::iterator itDecoys;
                for (itDecoys = mCtx.decoys.begin();
                        itDecoys != mCtx.decoys.end(); itDecoys++) {
                    molsToReduce.push_back(&(*itDecoys));
                }
                molsToReduce.push_back(&mCtx.source);
                molsToReduce.push_back(&mCtx.target);
                if (mCtx.ScaffoldMode()) {
                    std::vector<MolpherMolecule>::iterator itPathMols;
                    for (itPathMols = mCtx.pathMolecules.begin();
                            itPathMols != mCtx.pathMolecules.end(); itPathMols++) {
                        molsToReduce.push_back(&(*itPathMols));
                    }
                    molsToReduce.push_back(&mCtx.tempSource);
                }
                // reduce ..
                DimensionReducer *reducer =
                        ReducerFactory::Create(mCtx.dimRedSelector);
                reducer->Reduce(molsToReduce,
                        mCtx.fingerprintSelector, mCtx.simCoeffSelector, *mTbbCtx);
                ReducerFactory::Recycle(reducer);

                stageStopwatch.ReportElapsedMiliseconds("DimensionReduction", true);
            }

            if (!Cancelled()) {
                // find the closes molecule
                double distance = 1;
                PathFinderContext::CandidateMap::iterator itCandidates;
                for (itCandidates = mCtx.candidates.begin();
                        itCandidates != mCtx.candidates.end(); itCandidates++) {
                    if (itCandidates->second.distToTarget < distance) {
                        distance = itCandidates->second.distToTarget;
                    }

                    if (itCandidates->second.distToTarget == 0) {
                        std::stringstream ss;
                        ss << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": "
                                << "Zero distance: " << itCandidates->second.smile;
                        SynchCout(ss.str());
                    }
                }
                std::stringstream ss;
                ss << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": "
                        << "The min. distance to target: " << distance;
                SynchCout(ss.str());
            }

            if (!Cancelled()) {
                mCtx.iterIdx += 1;
                mCtx.elapsedSeconds += molpherStopwatch.GetElapsedSeconds();

                if (canContinueCurrentJob) {
                    bool itersDepleted = (mCtx.params.cntIterations <= mCtx.iterIdx);
                    bool timeDepleted = (mCtx.params.timeMaxSeconds <= mCtx.elapsedSeconds);
                    canContinueCurrentJob = (!itersDepleted && !timeDepleted);

                    if (itersDepleted) {
                        std::stringstream ss;
                        ss << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": "
                                << "The max number of iterations has been reached.";
                        SynchCout(ss.str());
                    }
                    if (timeDepleted) {
                        std::stringstream ss;
                        ss << mCtx.jobId << "/" << mCtx.iterIdx + 1 << ": "
                                << "We run out of time.";
                        SynchCout(ss.str());
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
/* Copyright 2015,2016 Tao Xu
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "TreeRegressor.h"

#include <cstdlib>
#include <limits>
#include <boost/random/uniform_real.hpp>

#include "Tree.h"
#include "GbmFun.h"
#include "DataSet.h"
#include "gflags/gflags.h"
#include "glog/logging.h"

DEFINE_int32(min_leaf_examples, 256,
             "minimum number of data points in the leaf");

namespace boosting {

using namespace std;

// Return true with approximately the desired probability;
// actual probability may differ by ~ 1/RAND_MAX
// Not thread safe. But:
// (1) we're only using this to sample some examples and features,
//     so we don't care (?)
// (2) currently TreeRegressor isn't multithreading anyway
inline bool biasedCoinFlip(double probabilityOfTrue) {
  return (rand() < probabilityOfTrue * RAND_MAX);
}

TreeRegressor::SplitNode::SplitNode(const vector<int>* st):
  subset(st), fid(-1), fv(0), gain(0), selected(false),
  left(NULL), right(NULL) {
}

TreeRegressor::TreeRegressor(
  const DataSet& ds,
  const boost::scoped_array<double>& y,
  const GbmFun& fun) : ds_(ds), y_(y), fun_(fun) {
}

TreeRegressor::~TreeRegressor() {
  for (SplitNode* split : allSplits_) {
    delete split;
  }
}

void TreeRegressor::splitExamples(
  const SplitNode& split,
  vector<int>* left,
  vector<int>* right) {

  const int fid = split.fid;
  const uint16_t fv = split.fv;

  auto &f = ds_.features_[fid];

  if (f.encoding == BYTE) {
    boosting::split<uint8_t>(*(split.subset), left, right, *(f.bvec), fv);
  } else {
    CHECK(f.encoding == SHORT);
    boosting::split<uint16_t>(*(split.subset), left, right, *(f.svec), fv);
  }
}

void TreeRegressor::getBestSplitFromHistogram(
  const TreeRegressor::Histogram& hist,
  int* idx,
  double* gain) {

  // The loss function should really be
  //   (sum of (y - y_mean)^2 for observations left of idx)
  //     + (sum of (y - y_mean)^2 for observations right of idx)
  // By math, this equals
  //   (sum of squares of all y-values)
  //     - (sum of y-values on left)^2 / (number of observations on left)
  //     - (sum of y-values on right)^2 / (number of observations on right)
  // Since the first term (sum of squares of all y-values) is independent
  // of our choice of where to split, it makes no difference, so we ignore it
  // in calculating loss.

  // loss function if we don't split at all
  double lossBefore = -1.0 * hist.totalSum * hist.totalSum / hist.totalCnt;

  int cntLeft = 0;       // number of observations on or to left of idx
  double sumLeft = 0.0;  // number of observations strictly to right of idx

  double bestGain = 0.0;
  int bestIdx = -1;      // everything strictly to right of idx

  CHECK(hist.num >= 1);

  for (int i = 0; i < hist.num - 1; i++) {

    cntLeft += hist.cnt[i];
    sumLeft += hist.sumy[i];

    double sumRight = hist.totalSum - sumLeft;
    int cntRight = hist.totalCnt - cntLeft;

    if (cntLeft < FLAGS_min_leaf_examples) {
      continue;
    }
    if (cntRight < FLAGS_min_leaf_examples) {
      break;
    }

    double lossAfter =
      -1.0 * sumLeft * sumLeft / cntLeft
      - 1.0 * sumRight * sumRight / cntRight;

    double gain = lossBefore - lossAfter;
    if (gain > bestGain) {
      bestGain = gain;
      bestIdx = i;
    }
  }

  *idx = bestIdx;
  *gain = bestGain;
}

TreeRegressor::SplitNode*
TreeRegressor::getBestSplit(const vector<int>* subset,
                            double featureSamplingRate,
                            bool terminal) {

  SplitNode* split = new SplitNode(subset);
  if (terminal) {
    allSplits_.push_back(split);
    return split;
  }

  int bestFid = -1;       // which feature to split on, -1 is invalid
  int bestFv = 0;         // critical value of that feature

  // gain in prediction accuracy from that split:
  // initialize to 0 instead of std::numeric_limits<double>::lowest() because,
  // if no split results in a positive gain, we would rather report that, than
  // return a valid but degenerate split
  double bestGain = 0.0;

  double totalSum = 0.0;  // sum of all target values

  for (auto& id : *subset) {
    totalSum += y_[id];
  }

  // For each of a random sampling of features, see if splitting on that
  // feature results in the biggest improvement so far.
  // TODO(tiankai): The various fid's can be processed in parallel.
  for (int fid = 0; fid < ds_.numFeatures_; fid++) {
    const auto& f = ds_.features_[fid];

    if (f.encoding == EMPTY || !biasedCoinFlip(featureSamplingRate)) {
      continue;
    }

    Histogram hist(f.transitions.size() + 1, subset->size(), totalSum);

    if (f.encoding == BYTE) {
      buildHistogram<uint8_t>(*subset, *(f.bvec), hist);
    } else {
      CHECK(f.encoding == SHORT);
      buildHistogram<uint16_t>(*subset, *(f.svec), hist);
    }

    int fv;
    double gain;
    getBestSplitFromHistogram(hist, &fv, &gain);

    if (gain > bestGain) {
      bestFid = fid;
      bestFv = fv;
      bestGain = gain;
    }
  }
  split->fid = bestFid;
  split->fv = bestFv;
  split->gain = bestGain;

  frontiers_.push_back(split);
  allSplits_.push_back(split);
  return split;
}


TreeNode<uint16_t>* TreeRegressor::getTree(
  const int numLeaves,
  const double exampleSamplingRate,
  const double featureSamplingRate,
  double fimps[]) {

  // randomly sample data in ds_
  vector<int>* subset = new vector<int>();
  for (int i = 0; i < ds_.getNumExamples(); i++) {
    if (biasedCoinFlip(exampleSamplingRate)) {
      subset->push_back(i);
    }
  }
  CHECK(subset->size() >= FLAGS_min_leaf_examples * numLeaves);

  // compute the decision tree in SplitNode's
  SplitNode* root = getBestSplits(subset, numLeaves - 1, featureSamplingRate);

  // convert the decision tree to PartitionNode's and LeafNode's
  return getTreeHelper(root, fimps);
}

TreeNode<uint16_t>* TreeRegressor::getTreeHelper(
  SplitNode* split,
  double fimps[]) {

  if (split == NULL) {
    return NULL;
  } else if (!split->selected) {
    // leaf of decision tree
    double fvote = fun_.getLeafVal(*(split->subset), y_);
    LOG(INFO) << "leaf:  " << fvote << ", #examples:"
              << split->subset->size();
    CHECK(split->subset->size() >= FLAGS_min_leaf_examples);

    return new LeafNode<uint16_t>(fvote);
  } else {
    // internal node of decision tree
    LOG(INFO) << "select split: " << split->fid << ":" << split->fv
              << " gain: " << split->gain << ", #examples:"
              << split->subset->size() << ", min partition: "
              << std::min(split->left->subset->size(), split->right->subset->size());

    fimps[split->fid] += split->gain;
    double fvote = fun_.getLeafVal(*(split->subset), y_);
    PartitionNode<uint16_t>* node = new PartitionNode<uint16_t>(split->fid, split->fv);
    node->setLeft(getTreeHelper(split->left, fimps));
    node->setRight(getTreeHelper(split->right, fimps));
    node->setVote(fvote);

    return node;
  }
}

TreeRegressor::SplitNode* TreeRegressor::getBestSplits(
  const vector<int>* subset, const int numSplits, double featureSamplingRate) {

  CHECK(subset != NULL);

  // Compute the root of the decision tree.
  SplitNode* firstSplit = getBestSplit(subset, featureSamplingRate, false);

  int numSelected = 0;
  do {
    // frontiers_.size() = #leaves = #internal nodes + 1 = numSelected + 1
    CHECK(frontiers_.size() == numSelected+1);

    // Do a linear search over the leaves to find the next split with the most
    // gain.
    double bestGain = 0.0;
    vector<SplitNode*>::iterator best_it = frontiers_.end();
    for (auto it = frontiers_.begin(); it != frontiers_.end(); it++) {
      if ((*it)->gain > bestGain) {
        bestGain = (*it)->gain;
        best_it = it;
      }
    }

    if (best_it == frontiers_.end()) {
      // no gain from any split
      break;
    }

    CHECK(bestGain > 0.0);

    (*best_it)->selected = true;
    numSelected++;
    SplitNode* bestSplit = *best_it;
    frontiers_.erase(best_it);

    // Now that we've selected bestSplit, expand its left and right children.
    vector<int>* left = new vector<int>();
    vector<int>* right = new vector<int>();

    splitExamples(*bestSplit, left, right);
    bool terminal = (numSelected == numSplits);

    bestSplit->left = getBestSplit(left, featureSamplingRate, terminal);
    bestSplit->right = getBestSplit(right, featureSamplingRate, terminal);
  } while (numSelected < numSplits);

  return firstSplit;
}

}

#ifndef RecoPixelVertexing_PixelVertexFinding_src_gpuClusterTracksDBSCAN_h
#define RecoPixelVertexing_PixelVertexFinding_src_gpuClusterTracksDBSCAN_h

#include <algorithm>
#include <execution>
#include <ranges>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "CUDACore/HistoContainer.h"

#include "gpuVertexFinder.h"

namespace gpuVertexFinder {

  // this algo does not really scale as it works in a single block...
  // enough for <10K tracks we have
  void clusterTracksDBSCAN(ZVertices* pdata,
                                      WorkSpace* pws,
                                      int minT,      // min number of neighbours to be "core"
                                      float eps,     // max absolute distance to cluster
                                      float errmax,  // max error to be "seed"
                                      float chi2max  // max normalized distance to cluster
  ) {
    constexpr bool verbose = false;  // in principle the compiler should optmize out if false

    if (verbose)
      printf("params %d %f %f %f\n", minT, eps, errmax, chi2max);

    auto er2mx = errmax * errmax;

    auto& __restrict__ data = *pdata;
    auto& __restrict__ ws = *pws;
    auto nt = ws.ntrks;
    float const* __restrict__ zt = ws.zt;
    float const* __restrict__ ezt2 = ws.ezt2;

    uint32_t& nvFinal = data.nvFinal;
    uint32_t& nvIntermediate = ws.nvIntermediate;

    uint8_t* __restrict__ izt = ws.izt;
    int32_t* __restrict__ nn = data.ndof;
    int32_t* __restrict__ iv = ws.iv;

    assert(pdata);
    assert(zt);

    using Hist = cms::cuda::HistoContainer<uint8_t, 256, 16000, 8, uint16_t>;
    auto hist_ptr{std::make_unique<Hist>()};
    Hist *hist{hist_ptr.get()};
    std::fill(std::execution::par, hist->off, hist->off + Hist::totbins(), 0);

    if (verbose)
      printf("booked hist with %d bins, size %d for %d tracks\n", hist->nbins(), hist->capacity(), nt);

    assert(nt <= hist->capacity());

    // fill hist  (bin shall be wider than "eps")
    auto iter_nt{std::views::iota(0U, nt)};
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      assert(i < ZVertices::MAXTRACKS);
      int iz = int(zt[i] * 10.);  // valid if eps<=0.1
      // iz = std::clamp(iz, INT8_MIN, INT8_MAX);  // sorry c++17 only
      iz = std::min(std::max(iz, INT8_MIN), INT8_MAX);
      izt[i] = iz - INT8_MIN;
      assert(iz - INT8_MIN >= 0);
      assert(iz - INT8_MIN < 256);
      hist->count(izt[i]);
      iv[i] = i;
      nn[i] = 0;
    });
    hist->finalize();
    assert(hist->size() == nt);
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      hist->fill(izt[i], uint16_t(i));
    });

    // count neighbours
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (ezt2[i] <= er2mx){
        auto loop = [&](uint32_t j) {
          if (i == j)
            return;
          auto dist = std::abs(zt[i] - zt[j]);
          if (dist > eps)
            return;
          //        if (dist*dist>chi2max*(ezt2[i]+ezt2[j])) return;
          nn[i]++;
        };

        cms::cuda::forEachInBins(*hist, izt[i], 1, loop);
      }
    });

    // find NN with smaller z...
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (nn[i] >= minT){
        float mz = zt[i];
        auto loop = [&](uint32_t j) {
          if (zt[j] >= mz)
            return;
          if (nn[j] < minT)
            return;  // DBSCAN core rule
          auto dist = std::abs(zt[i] - zt[j]);
          if (dist > eps)
            return;
          //        if (dist*dist>chi2max*(ezt2[i]+ezt2[j])) return;
          mz = zt[j];
          iv[i] = j;  // assign to cluster (better be unique??)
        };
        cms::cuda::forEachInBins(*hist, izt[i], 1, loop);
      }
    });

#ifdef GPU_DEBUG
    //  mini verification
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (iv[i] != int(i))
        assert(iv[iv[i]] != int(i));
    });
#endif

    // consolidate graph (percolate index of seed)
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      auto m = iv[i];
      while (m != iv[m])
        m = iv[m];
      iv[i] = m;
    });

#ifdef GPU_DEBUG
    //  mini verification
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (iv[i] != int(i))
        assert(iv[iv[i]] != int(i));
    });
#endif

#ifdef GPU_DEBUG
    // and verify that we did not spit any cluster...
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (nn[i] >= minT){
        assert(zt[iv[i]] <= zt[i]);
        auto loop = [&](uint32_t j) {
          if (nn[j] < minT)
            return;  // DBSCAN core rule
          auto dist = std::abs(zt[i] - zt[j]);
          if (dist > eps)
            return;
          //  if (dist*dist>chi2max*(ezt2[i]+ezt2[j])) return;
          // they should belong to the same cluster, isn't it?
          if (iv[i] != iv[j]) {
            printf("ERROR %d %d %f %f %d\n", i, iv[i], zt[i], zt[iv[i]], iv[iv[i]]);
            printf("      %d %d %f %f %d\n", j, iv[j], zt[j], zt[iv[j]], iv[iv[j]]);
            ;
          }
          assert(iv[i] == iv[j]);
        };
        cms::cuda::forEachInBins(*hist, izt[i], 1, loop);
      }
    });
#endif

    // collect edges (assign to closest cluster of closest point??? here to closest point)
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      //    if (nn[i]==0 || nn[i]>=minT) continue;    // DBSCAN edge rule
      if (nn[i] < minT){
        float mdist = eps;
        auto loop = [&](uint32_t j) {
          if (nn[j] < minT)
            return;  // DBSCAN core rule
          auto dist = std::abs(zt[i] - zt[j]);
          if (dist > mdist)
            return;
          if (dist * dist > chi2max * (ezt2[i] + ezt2[j]))
            return;  // needed?
          mdist = dist;
          iv[i] = iv[j];  // assign to cluster (better be unique??)
        };
        cms::cuda::forEachInBins(*hist, izt[i], 1, loop);
      }
    });

    std::unique_ptr<unsigned int> foundClusters_ptr{std::make_unique<unsigned int>(0)};
    unsigned int* foundClusters{foundClusters_ptr.get()};

    // find the number of different clusters, identified by a tracks with clus[i] == i;
    // mark these tracks with a negative id.
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (iv[i] == int(i)) {
        if (nn[i] >= minT) {
          std::atomic_ref<unsigned int> inc(*foundClusters);
          auto old = inc.fetch_add(0xffffffff);
          iv[i] = -(old + 1);
        } else {  // noise
          iv[i] = -9998;
        }
      }
    });

    assert(*foundClusters < ZVertices::MAXVTX);

    // propagate the negative id to all the tracks in the cluster.
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      if (iv[i] >= 0) {
        // mark each track in a cluster with the same id as the first one
        iv[i] = iv[iv[i]];
      }
    });

    // adjust the cluster id to be a positive value starting from 0
    std::for_each(std::execution::par, std::ranges::cbegin(iter_nt), std::ranges::cend(iter_nt), [=](const auto i) {
      iv[i] = -iv[i] - 1;
    });

    nvIntermediate = nvFinal = *foundClusters;

    if (verbose)
      printf("found %d proto vertices\n", *foundClusters);
  }

}  // namespace gpuVertexFinder

#endif  // RecoPixelVertexing_PixelVertexFinding_src_gpuClusterTracksDBSCAN_h

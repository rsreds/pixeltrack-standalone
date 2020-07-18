#ifndef RecoPixelVertexing_PixelVertexFinding_src_gpuSortByPt2_h
#define RecoPixelVertexing_PixelVertexFinding_src_gpuSortByPt2_h

#include "gpuVertexFinder.h"

namespace KOKKOS_NAMESPACE {
  namespace gpuVertexFinder {

    using team_policy = Kokkos::TeamPolicy<KokkosExecSpace>;
    using member_type = Kokkos::TeamPolicy<KokkosExecSpace>::member_type;

    KOKKOS_INLINE_FUNCTION void sortByPt2(Kokkos::View<ZVertices, KokkosExecSpace> vdata,
                                          Kokkos::View<WorkSpace, KokkosExecSpace> vws,
                                          const Kokkos::TeamPolicy<KokkosExecSpace>::member_type& team_member) {
      auto& __restrict__ data = *vdata.data();
      auto& __restrict__ ws = *vws.data();
      auto nt = ws.ntrks;
      float const* __restrict__ ptt2 = ws.ptt2;
      uint32_t const& nvFinal = data.nvFinal;

      int32_t const* __restrict__ iv = ws.iv;
      float* __restrict__ ptv2 = data.ptv2;
      uint16_t* __restrict__ sortInd = data.sortInd;

      // if (threadIdx.x == 0)
      //    printf("sorting %d vertices\n",nvFinal);

      if (nvFinal < 1)
        return;

      // fill indexing
      for (unsigned int i = team_member.team_rank(); i < nt; i += team_member.team_size()) {
        data.idv[ws.itrk[i]] = iv[i];
      }

      // can be done asynchronoisly at the end of previous event
      for (unsigned int i = team_member.team_rank(); i < nvFinal; i += team_member.team_size()) {
        ptv2[i] = 0;
      }
      team_member.team_barrier();

      for (unsigned int i = team_member.team_rank(); i < nt; i += team_member.team_size()) {
        if (iv[i] > 9990)
          continue;
        Kokkos::atomic_add(&ptv2[iv[i]], ptt2[i]);
      }

      team_member.team_barrier();

      if (1 == nvFinal) {
        if (team_member.team_rank() == 0)
          sortInd[0] = 0;
        return;
      }
    }

    KOKKOS_INLINE_FUNCTION void sortByPt2Kernel(Kokkos::View<ZVertices, KokkosExecSpace> vdata,
                                                Kokkos::View<WorkSpace, KokkosExecSpace> vws,
                                                const Kokkos::TeamPolicy<KokkosExecSpace>::member_type& team_member) {
      Kokkos::abort("sortByPt2Kernel: device sort kernel not supported in Kokkos (see sortByPt2Host)");
    }

    // equivalent to sortByPt2Kernel + deep copy to host
    void sortByPt2Host(Kokkos::View<ZVertices, KokkosExecSpace> vdata,
                       Kokkos::View<WorkSpace, KokkosExecSpace> vws,
                       typename Kokkos::View<ZVertices, KokkosExecSpace>::HostMirror hdata,
                       const team_policy& policy) {
      Kokkos::parallel_for(
          policy, KOKKOS_LAMBDA(const member_type& team_member) { sortByPt2(vdata, vws, team_member); });
      Kokkos::deep_copy(KokkosExecSpace(), hdata, vdata);

      auto& __restrict__ data = *hdata.data();
      uint32_t const& nvFinal = data.nvFinal;
      float* __restrict__ ptv2 = data.ptv2;
      uint16_t* __restrict__ sortInd = data.sortInd;

      for (uint16_t i = 0; i < nvFinal; ++i)
        sortInd[i] = i;
      std::sort(sortInd, sortInd + nvFinal, [&](auto i, auto j) { return ptv2[i] < ptv2[j]; });
    }

  }  // namespace gpuVertexFinder
}  // namespace KOKKOS_NAMESPACE

#endif  // RecoPixelVertexing_PixelVertexFinding_src_gpuSortByPt2_h

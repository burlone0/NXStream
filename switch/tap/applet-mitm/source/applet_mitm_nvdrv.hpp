#pragma once
#include <stratosphere.hpp>

#define AMS_NVDRV_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H,  0, Result, Open,   (sf::Out<u32> out_fd, sf::Out<u32> out_err, const sf::InBuffer &path), (out_fd, out_err, path)) \
    AMS_SF_METHOD_INFO(C, H,  1, Result, Ioctl,  (sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out), (out_err, fd, rq, in, out)) \
    AMS_SF_METHOD_INFO(C, H,  2, Result, Close,  (sf::Out<u32> out_err, u32 fd), (out_err, fd)) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, Ioctl2, (sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::InAutoSelectBuffer &in2, const sf::OutAutoSelectBuffer &out), (out_err, fd, rq, in, in2, out)) \
    AMS_SF_METHOD_INFO(C, H, 12, Result, Ioctl3, (sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out, const sf::OutAutoSelectBuffer &out2), (out_err, fd, rq, in, out, out2))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, INvDrvMitm, AMS_NVDRV_MITM_INTERFACE_INFO, 0x4E564452)

namespace ams::mitm::applet {

    constexpr u64 GrcProgramId = 0x0100000000000035ull;
    extern bool g_grc_armed;

    void NoteNvdrvQuery(u64 program_id);

    class NvDrvMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                NoteNvdrvQuery(client_info.program_id.value);
                return g_grc_armed && client_info.program_id.value == GrcProgramId;
            }
        private:
            enum FdKind : u8 { Fd_Unknown = 0, Fd_Nvmap, Fd_Msenc, Fd_Ctrl, Fd_Other };
            static constexpr size_t MaxFds = 64, MaxHandles = 256;
            FdKind m_fd_kind[MaxFds] = {};
            struct Hnd { u32 handle, size, iova; u64 addr; };
            Hnd m_h[MaxHandles] = {};
            u32 m_nh = 0;
            Hnd *FindHandle(u32 h, bool create);
            FdKind KindOf(u32 fd) const { return fd < MaxFds ? m_fd_kind[fd] : Fd_Unknown; }
            void DumpSubmit(const u8 *in, size_t len, u32 fd);
        public:
            Result Open(sf::Out<u32> out_fd, sf::Out<u32> out_err, const sf::InBuffer &path);
            Result Ioctl(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out);
            Result Close(sf::Out<u32> out_err, u32 fd);
            Result Ioctl2(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::InAutoSelectBuffer &in2, const sf::OutAutoSelectBuffer &out);
            Result Ioctl3(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out, const sf::OutAutoSelectBuffer &out2);
    };
    static_assert(IsINvDrvMitm<NvDrvMitm>);

    void FlushNvdrvRecords();

}

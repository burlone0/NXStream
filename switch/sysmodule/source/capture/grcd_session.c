#include "capture/grcd_session.h"

Result grcdSessionOpen(Service* srv) { return smGetService(srv, "grc:d"); }

Result grcdSessionBegin(Service* srv) { return serviceDispatch(srv, 1); }

Result grcdSessionTransfer(Service* srv, GrcStream stream, void* buffer, size_t size, u32* num_frames,
                           u32* data_size, u64* start_timestamp) {
    struct {
        u32 num_frames;
        u32 data_size;
        u64 start_timestamp;
    } out;

    u32 tmp = stream;
    Result rc = serviceDispatchInOut(srv, 2, tmp, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } },
    );
    if (R_SUCCEEDED(rc)) {
        if (num_frames) *num_frames = out.num_frames;
        if (data_size) *data_size = out.data_size;
        if (start_timestamp) *start_timestamp = out.start_timestamp;
    }
    return rc;
}

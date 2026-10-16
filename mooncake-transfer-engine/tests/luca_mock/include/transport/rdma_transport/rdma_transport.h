#pragma once
#include "transport/transport.h"
#include <stdexcept>
namespace mooncake {
class RdmaTransport : public Transport {
   public:
    Status submitTransfer(BatchID,
                          const std::vector<TransferRequest>&) override {
        return Status::OK();
    }
    Status submitTransferTask(
        const std::vector<TransferTask*>& tasks) override {
        for (auto* task : tasks) {
            auto* slice = getSliceCache().allocate();
            slice->task = task;
            slice->length = task->request->length;
            slice->ts = 0;
            task->slice_list.push_back(slice);
            task->slice_count = 1;
            slice->markSuccess();
        }
        return Status::OK();
    }
    Status getTransferStatus(BatchID, size_t, TransferStatus&) override {
        throw std::runtime_error("Counter-only backend unexpectedly polled");
    }

   private:
    int registerLocalMemory(void*, size_t, const std::string&, bool,
                            bool) override {
        return 0;
    }
    int unregisterLocalMemory(void*, bool) override { return 0; }
    int registerLocalMemoryBatch(const std::vector<BufferEntry>&,
                                 const std::string&) override {
        return 0;
    }
    int unregisterLocalMemoryBatch(const std::vector<void*>&) override {
        return 0;
    }
    const char* getName() const override { return "rdma"; }
};
}  // namespace mooncake

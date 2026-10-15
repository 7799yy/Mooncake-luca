// Experimental, synchronous LUCA transport for the supplied Mooncake revision.
#ifndef MOONCAKE_LUCA_TRANSPORT_H_
#define MOONCAKE_LUCA_TRANSPORT_H_
#include <mutex>
#include "transport/transport.h"

namespace mooncake {
class LucaTransport : public Transport {
 public:
    Status submitTransfer(BatchID, const std::vector<TransferRequest>&) override;
    Status submitTransferTask(const std::vector<TransferTask*>&) override;
    Status getTransferStatus(BatchID, size_t, TransferStatus&) override;
 protected:
    int install(std::string&, std::shared_ptr<TransferMetadata>,
                std::shared_ptr<Topology>) override;
 private:
    int registerLocalMemory(void*, size_t, const std::string&, bool,
                            bool = true) override;
    int unregisterLocalMemory(void*, bool = true) override;
    int registerLocalMemoryBatch(const std::vector<BufferEntry>&,
                                 const std::string&) override;
    int unregisterLocalMemoryBatch(const std::vector<void*>&) override;
    const char* getName() const override { return "luca"; }
    bool transferOne(const TransferRequest&);
    std::mutex mutex_;
    std::string host_id_;
};
}  // namespace mooncake
#endif

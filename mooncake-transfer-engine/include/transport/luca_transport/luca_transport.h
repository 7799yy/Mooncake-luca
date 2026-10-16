// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// MACA-derived single-visible-GPU LUCA Cross v2 transport.
#ifndef MOONCAKE_LUCA_TRANSPORT_H_
#define MOONCAKE_LUCA_TRANSPORT_H_
#include <memory>
#include "transport/transport.h"

namespace mooncake {
class LucaTransport : public Transport {
   public:
    LucaTransport();
    ~LucaTransport() override;
    Status submitTransfer(BatchID,
                          const std::vector<TransferRequest>&) override;
    Status submitTransferTask(const std::vector<TransferTask*>&) override;
    Status getTransferStatus(BatchID, size_t, TransferStatus&) override;
    bool requiresStatusPolling() const override { return true; }

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mooncake
#endif

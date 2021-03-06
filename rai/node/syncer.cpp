#include <rai/node/node.hpp>
#include <rai/node/syncer.hpp>

rai::SyncStat::SyncStat() : total_(0), miss_(0)
{
}

void rai::SyncStat::Reset()
{
    total_ = 0;
    miss_  = 0;
}

rai::Syncer::Syncer(rai::Node& node) : node_(node), current_query_id_(0)
{
    node_.observers_.block_.Add(
        [this](const rai::BlockProcessResult& result,
               const std::shared_ptr<rai::Block>& block) {
            this->ProcessorCallback(result, block);
        });
}

void rai::Syncer::Add(const rai::Account& account, uint64_t height, bool stat,
                      uint32_t batch_id)
{
    Add(account, height, rai::BlockHash(0), stat, batch_id);
}

void rai::Syncer::Add(const rai::Account& account, uint64_t height,
                      const rai::BlockHash& previous, bool stat,
                      uint32_t batch_id)
{
    rai::SyncInfo info{rai::SyncStatus::QUERY, stat, batch_id, height, previous,
                       rai::BlockHash(0)};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool error = Add_(account, info);
        if (error)
        {
            return;
        }

        if (stat)
        {
            ++stat_.total_;
        }
    }

    BlockQuery_(account, info, batch_id);
}

uint64_t rai::Syncer::AddQuery(uint32_t batch_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t query_id = current_query_id_++;
    while (true)
    {
        auto it = queries_.find(query_id);
        if (it != queries_.end())
        {
            query_id = current_query_id_++;
            continue;
        }
        queries_.emplace(query_id, batch_id);
        break;
    }

    return query_id;
}

uint32_t rai::Syncer::BatchId(uint64_t query_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queries_.find(query_id);
    if (it != queries_.end())
    {
        return it->second;
    }
    return rai::Syncer::DEFAULT_BATCH_ID;
}

bool rai::Syncer::Busy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return syncs_.size() >= rai::Syncer::BUSY_SIZE;
}

bool rai::Syncer::Empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return syncs_.size() == 0;
}

void rai::Syncer::Erase(const rai::Account& account)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = syncs_.find(account);
    if (it != syncs_.end())
    {
        syncs_.erase(it);
    }
}

void rai::Syncer::EraseQuery(uint64_t query_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    queries_.erase(query_id);
}

bool rai::Syncer::Exists(const rai::Account& account) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return syncs_.find(account) != syncs_.end();
}

bool rai::Syncer::Finished(uint32_t batch_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& i : syncs_)
    {
        if (i.second.batch_id_ == batch_id)
        {
            return false;
        }
    }

    for (const auto& i : queries_)
    {
        if (i.second == batch_id)
        {
            return false;
        }
    }

    return true;
}


void rai::Syncer::ProcessorCallback(const rai::BlockProcessResult& result,
                                    const std::shared_ptr<rai::Block>& block)
{
    if (result.operation_ != rai::BlockOperation::APPEND
        && result.operation_ != rai::BlockOperation::DROP)
    {
        return;
    }

    rai::SyncInfo info;
    uint32_t batch_id = rai::Syncer::DEFAULT_BATCH_ID;
    bool query        = false;
    bool source_miss  = false;
    bool sync_related = false;
    do
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = syncs_.find(block->Account());
        if (it == syncs_.end()
            || it->second.status_ != rai::SyncStatus::PROCESS)
        {
            return;
        }

        if (it->second.current_ != block->Hash())
        {
            return;
        }

        if (result.operation_ == rai::BlockOperation::DROP)
        {
            it->second.status_  = rai::SyncStatus::QUERY;
            it->second.current_ = rai::BlockHash(0);
            info                = it->second;
            query               = true;
            batch_id            = info.batch_id_;
            break;
        }

        // APPEND
        if (result.error_code_ == rai::ErrorCode::SUCCESS
            || result.error_code_ == rai::ErrorCode::BLOCK_PROCESS_EXISTS)
        {
            it->second.status_   = rai::SyncStatus::QUERY;
            it->second.current_  = rai::BlockHash(0);
            it->second.height_   = block->Height() + 1;
            it->second.previous_ = block->Hash();
            info                 = it->second;
            query                = true;
            sync_related         = true;
            batch_id             = info.batch_id_;
        }
        else if (result.error_code_
                     == rai::ErrorCode::BLOCK_PROCESS_GAP_RECEIVE_SOURCE
                 || result.error_code_
                        == rai::ErrorCode::BLOCK_PROCESS_GAP_REWARD_SOURCE
                 || result.error_code_
                        == rai::ErrorCode::BLOCK_PROCESS_UNREWARDABLE)
        {
            source_miss = true;
            batch_id = it->second.batch_id_;
            syncs_.erase(it);
        }
        else
        {
            syncs_.erase(it);
            return;
        }
    } while (0);

    if (query)
    {
        BlockQuery_(block->Account(), info, batch_id);
    }

    if (source_miss)
    {
        BlockQuery_(block->Link(), batch_id);
    }

    if (sync_related)
    {
        SyncRelated(block, batch_id);
    }
}

void rai::Syncer::QueryCallback(const rai::Account& account,
                                rai::QueryStatus status,
                                const std::shared_ptr<rai::Block>& block)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = syncs_.find(account);
        if (it == syncs_.end())
        {
            return;
        }

        if (it->second.status_ != rai::SyncStatus::QUERY)
        {
            return;
        }

        if (status == rai::QueryStatus::MISS)
        {
            if (it->second.first_)
            {
                ++stat_.miss_;
            }
            syncs_.erase(it);
            return;
        }
        else if (status == rai::QueryStatus::SUCCESS)
        {
            it->second.first_   = false;
            it->second.status_  = rai::SyncStatus::PROCESS;
            it->second.current_ = block->Hash();
            assert(it->second.height_ == block->Height());
        }
        else if (status == rai::QueryStatus::FORK)
        {
            syncs_.erase(it);
        }
        else
        {
            assert(0);
            syncs_.erase(it);
            return;
        }
    }

    node_.block_processor_.Add(block);
}

rai::SyncStat rai::Syncer::Stat() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stat_;
}

void rai::Syncer::ResetStat()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stat_.Reset();
    for (auto it = syncs_.begin(); it != syncs_.end(); ++it)
    {
        it->second.first_ = false;
    }
}

size_t rai::Syncer::Size() const
{
    std::lock_guard<std::mutex> lock(mutex_);  
    return syncs_.size();
}

size_t rai::Syncer::Queries() const
{
    std::lock_guard<std::mutex> lock(mutex_);  
    return queries_.size();
}

void rai::Syncer::SyncAccount(rai::Transaction& transaction,
                              const rai::Account& account, uint32_t batch_id)
{
    rai::AccountInfo account_info;
    bool error =
        node_.ledger_.AccountInfoGet(transaction, account, account_info);
    if (error || !account_info.Valid())
    {
        node_.syncer_.Add(account, 0, false, batch_id);
    }
    else
    {
        node_.syncer_.Add(account, account_info.head_height_ + 1,
                            account_info.head_, false, batch_id);
    }
}

void rai::Syncer::SyncRelated(const std::shared_ptr<rai::Block>& block,
                              uint32_t batch_id)
{
    if (!block->HasRepresentative()
        && block->Opcode() != rai::BlockOpcode::SEND)
    {
        return;
    }

    rai::ErrorCode error_code = rai::ErrorCode::SUCCESS;
    rai::Transaction transaction(error_code, node_.ledger_, false);
    if (error_code != rai::ErrorCode::SUCCESS)
    {
        // log
        return;
    }

    if (block->Opcode() == rai::BlockOpcode::SEND)
    {
        SyncAccount(transaction, block->Link(), batch_id);
    }

    if (block->HasRepresentative() && block->Height() > 0)
    {
        rai::Account rep = block->Representative();
        std::shared_ptr<rai::Block> previous(nullptr);
        if (block->Opcode() == rai::BlockOpcode::CHANGE)
        {
            bool error = node_.ledger_.BlockGet(transaction, block->Previous(),
                                                previous);
            if (error)
            {
                return;
            }
            rep = previous->Representative();
        }

        rai::RewardableInfo info;
        bool error = node_.ledger_.RewardableInfoGet(transaction, rep,
                                                     block->Previous(), info);
        if (error || info.valid_timestamp_ > rai::CurrentTimestamp())
        {
            return;
        }
        SyncAccount(transaction, rep, batch_id);
    }
}

bool rai::Syncer::Add_(const rai::Account& account, const rai::SyncInfo& info)
{
    auto it = syncs_.find(account);
    if (it != syncs_.end())
    {
        return true;
    }

    syncs_[account] = info;
    return false;
}

void rai::Syncer::BlockQuery_(const rai::Account& account,
                              const rai::SyncInfo& info, uint32_t batch_id)
{
    uint64_t query_id = AddQuery(batch_id);
    if (info.height_ == 0 || info.previous_.IsZero())
    {
        node_.block_queries_.QueryByHeight(
            account, info.height_, false,
            QueryCallbackByAccount_(account, query_id));
    }
    else
    {
        node_.block_queries_.QueryByPrevious(
            account, info.height_, info.previous_, false,
            QueryCallbackByAccount_(account, query_id));
    }
}

void rai::Syncer::BlockQuery_(const rai::BlockHash& hash, uint32_t batch_id)
{
    uint64_t query_id = AddQuery(batch_id);
    node_.block_queries_.QueryByHash(rai::Account(0),
                                     rai::Block::INVALID_HEIGHT, hash, true,
                                     QueryCallbackByHash_(hash, query_id));
}

rai::QueryCallback rai::Syncer::QueryCallbackByAccount_(
    const rai::Account& account, uint64_t query_id)
{
    std::weak_ptr<rai::Node> node_w(node_.Shared());
    rai::QueryCallback callback = [node_w, account, query_id, count = 0](
                                      const std::vector<rai::QueryAck>& acks,
                                      std::vector<rai::QueryCallbackStatus>&
                                          result) mutable {
        auto node(node_w.lock());
        if (!node)
        {
            result.insert(result.end(), acks.size(),
                          rai::QueryCallbackStatus::FINISH);
            return;
        }

        if (acks.size() != 1)
        {
            result.insert(result.end(), acks.size(),
                          rai::QueryCallbackStatus::FINISH);
            node->syncer_.Erase(account);
            node->syncer_.EraseQuery(query_id);
            return;
        }

        auto& ack = acks[0];
        if (ack.status_ == rai::QueryStatus::FORK
            || ack.status_ == rai::QueryStatus::SUCCESS)
        {
            result.insert(result.end(), 1, rai::QueryCallbackStatus::FINISH);
            node->syncer_.QueryCallback(account, ack.status_, ack.block_);
            node->syncer_.EraseQuery(query_id);
        }
        else if (ack.status_ == rai::QueryStatus::MISS)
        {
            ++count;
            if (count >= 5)
            {
                result.insert(result.end(), 1,
                              rai::QueryCallbackStatus::FINISH);
                node->syncer_.QueryCallback(account, ack.status_, ack.block_);
                node->syncer_.EraseQuery(query_id);
            }
            else
            {
                result.insert(result.end(), 1,
                              rai::QueryCallbackStatus::CONTINUE);
            }
        }
        else if (ack.status_ == rai::QueryStatus::PRUNED
                 || ack.status_ == rai::QueryStatus::TIMEOUT)
        {
            result.insert(result.end(), 1, rai::QueryCallbackStatus::CONTINUE);
        }
        else
        {
            result.insert(result.end(), 1, rai::QueryCallbackStatus::FINISH);
            node->syncer_.Erase(account);
            node->syncer_.EraseQuery(query_id);
        }
    };
    return callback;
}

rai::QueryCallback rai::Syncer::QueryCallbackByHash_(const rai::BlockHash& hash,
                                                     uint64_t query_id)
{
    std::weak_ptr<rai::Node> node_w(node_.Shared());
    rai::QueryCallback callback = [node_w, query_id, count = 0](
                                      const std::vector<rai::QueryAck>& acks,
                                      std::vector<rai::QueryCallbackStatus>&
                                          result) mutable {
        auto node(node_w.lock());
        if (!node)
        {
            result.insert(result.end(), acks.size(),
                          rai::QueryCallbackStatus::FINISH);
            return;
        }

        if (acks.size() != 1)
        {
            result.insert(result.end(), acks.size(),
                          rai::QueryCallbackStatus::FINISH);
            node->syncer_.EraseQuery(query_id);
            return;
        }

        auto& ack = acks[0];
        if (ack.status_ == rai::QueryStatus::SUCCESS)
        {
            result.insert(result.end(), 1, rai::QueryCallbackStatus::FINISH);

            
            rai::ErrorCode error_code = rai::ErrorCode::SUCCESS;
            rai::Transaction transaction(error_code, node->ledger_, false);
            if (error_code != rai::ErrorCode::SUCCESS)
            {
                rai::Stats::Add(error_code, "Syncer::QueryCallbackByHash_");
                node->syncer_.EraseQuery(query_id);
                return;
            }

            uint32_t batch_id = node->syncer_.BatchId(query_id);
            node->syncer_.SyncAccount(transaction, ack.block_->Account(),
                                      batch_id);
            node->syncer_.EraseQuery(query_id);
        }
        else if (ack.status_ == rai::QueryStatus::MISS)
        {
            ++count;
            if (count >= 5)
            {
                result.insert(result.end(), 1,
                              rai::QueryCallbackStatus::FINISH);
                node->syncer_.EraseQuery(query_id);
            }
            else
            {
                result.insert(result.end(), 1,
                              rai::QueryCallbackStatus::CONTINUE);
            }
        }
        else if (ack.status_ == rai::QueryStatus::TIMEOUT
                 || ack.status_ == rai::QueryStatus::FORK
                 || ack.status_ == rai::QueryStatus::PRUNED)
        {
            result.insert(result.end(), 1, rai::QueryCallbackStatus::CONTINUE);
        }
        else
        {
            result.insert(result.end(), 1, rai::QueryCallbackStatus::FINISH);
            node->syncer_.EraseQuery(query_id);
        }
    };
    return callback;
}

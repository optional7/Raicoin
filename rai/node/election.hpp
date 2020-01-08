#pragma once
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>
#include <condition_variable>
#include <rai/common/blocks.hpp>
#include <rai/common/numbers.hpp>
#include <thread>
#include <unordered_map>
#include <unordered_set>


namespace rai
{
class Vote
{
public:
    Vote();
    Vote(uint64_t, const rai::Signature&, const rai::BlockHash&);

    uint64_t timestamp_;
    rai::Signature signature_;
    rai::BlockHash hash_;
};

class RepVoteInfo
{
public:
    RepVoteInfo();
    RepVoteInfo(bool, const rai::Amount&, const rai::Vote&);

    bool conflict_found_;
    rai::Amount weight_;
    rai::Vote last_vote_;
};

class BlockReference
{
public:
    uint32_t refs_;
    std::shared_ptr<rai::Block> block_;
};

class Election
{
public:
    Election();

    void AddBlock(const std::shared_ptr<rai::Block>&);
    void DelBlock(const rai::BlockHash&);
    bool ForkFound() const;

    rai::Account account_;
    uint64_t height_;
    bool fork_found_;
    uint32_t rounds_;
    uint32_t rounds_fork_;
    uint32_t wins_;
    uint32_t confirms_;
    rai::BlockHash winner_;
    std::chrono::steady_clock::time_point wakeup_;
    std::unordered_map<rai::BlockHash, rai::BlockReference> blocks_;
    std::unordered_map<rai::Account, rai::RepVoteInfo> votes_;
    std::unordered_map<rai::Account, rai::Vote> conflicts_;
};

class ElectionStatus
{
public:
    ElectionStatus();

    bool error_;
    bool win_;
    bool confirm_;
    std::shared_ptr<rai::Block> block_;
};

class Node;
class Elections
{
public:
    Elections(rai::Node&);
    ~Elections();
    void Add(const std::vector<std::shared_ptr<rai::Block>>&);
    void Run();
    void Stop();
    void ProcessElection(const rai::Election& election);
    void ProcessConfirm(const rai::Account&, uint64_t, const rai::Signature&,
                        const std::shared_ptr<rai::Block>&, const rai::Amount&);
    void ProcessConflict(const rai::Account&, uint64_t, uint64_t,
                         const rai::Signature&, const rai::Signature&,
                         const std::shared_ptr<rai::Block>&,
                         const std::shared_ptr<rai::Block>&,
                         const rai::Amount&);

    static std::chrono::seconds constexpr FORK_ELECTION_DELAY =
        std::chrono::seconds(60);
    static std::chrono::seconds constexpr FORK_ELECTION_INTERVAL =
        std::chrono::seconds(30);
    static std::chrono::seconds constexpr NON_FORK_ELECTION_DELAY =
        std::chrono::seconds(1);
    static std::chrono::seconds constexpr NON_FORK_ELECTION_INTERVAL =
        std::chrono::seconds(1);

private:
    void AddBlock_(const rai::Election&, const std::shared_ptr<rai::Block>&);
    void DelBlock_(const rai::Election&, const rai::BlockHash&);
    bool GetBlock_(const rai::Election&, const rai::BlockHash&,
                   std::shared_ptr<rai::Block>&) const;
    void AddConflict_(const rai::Election&, const rai::Account&,
                      const rai::Vote&);
    bool GetConflict_(const rai::Election&, const rai::Account&,
                      rai::Vote&) const;
    void AddRepVoteInfo_(const rai::Election&, const rai::Account&,
                         const rai::RepVoteInfo&);
    void ModifyRounds_(const rai::Election&, uint32_t);
    void ModifyRoundsFork_(const rai::Election&, uint32_t);
    void ModifyWins_(const rai::Election&, uint32_t);
    void ModifyConfirms_(const rai::Election&, uint32_t);
    void ModifyWinner_(const rai::Election&, const rai::BlockHash&);
    void ModifyWakeup_(const rai::Election&,
                       const std::chrono::steady_clock::time_point&);
    bool CheckConflict_(const rai::Vote&, const rai::Vote&) const;
    rai::ElectionStatus Tally_(
        const rai::Election&, const rai::Amount&,
        const std::unordered_map<rai::Account, rai::Amount>&) const;
    void RequestConfirms_(const rai::Election&);
    void BroadcastConfirms_(const rai::Election&);
    std::chrono::steady_clock::time_point NextWakeup_(
        const rai::Election&) const;

    rai::Node& node_;
    mutable std::mutex mutex_;
    boost::multi_index_container<
        Election,
        boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<boost::multi_index::member<
                Election, rai::Account, &Election::account_>>,
            boost::multi_index::ordered_non_unique<boost::multi_index::member<
                Election, std::chrono::steady_clock::time_point,
                &Election::wakeup_>>>>
        elections_;
    bool stopped_;

    std::condition_variable condition_;
    std::thread thread_;
};
}  // namespace rai
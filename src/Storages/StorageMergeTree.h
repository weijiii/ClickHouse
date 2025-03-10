#pragma once

#include <Core/Names.h>
#include <Storages/AlterCommands.h>
#include <Storages/IStorage.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <Storages/MergeTree/MergeTreeDataWriter.h>
#include <Storages/MergeTree/MergeTreeDataMergerMutator.h>
#include <Storages/MergeTree/MergeTreePartsMover.h>
#include <Storages/MergeTree/MergeTreeMutationEntry.h>
#include <Storages/MergeTree/MergeTreeMutationStatus.h>
#include <Storages/MergeTree/MergeTreeDeduplicationLog.h>
#include <Storages/MergeTree/FutureMergedMutatedPart.h>
#include <Storages/MergeTree/MergePlainMergeTreeTask.h>
#include <Storages/MergeTree/MutatePlainMergeTreeTask.h>

#include <Disks/StoragePolicy.h>
#include <Common/SimpleIncrement.h>


namespace DB
{

/** See the description of the data structure in MergeTreeData.
  */
class StorageMergeTree final : public MergeTreeData
{
public:
    /** Attach the table with the appropriate name, along the appropriate path (with / at the end),
      *  (correctness of names and paths are not checked)
      *  consisting of the specified columns.
      *
      * See MergeTreeData constructor for comments on parameters.
      */
    StorageMergeTree(
        const StorageID & table_id_,
        const String & relative_data_path_,
        const StorageInMemoryMetadata & metadata,
        bool attach,
        ContextMutablePtr context_,
        const String & date_column_name,
        const MergingParams & merging_params_,
        std::unique_ptr<MergeTreeSettings> settings_,
        bool has_force_restore_data_flag);

    void startup() override;
    void flush() override;
    void shutdown() override;

    ~StorageMergeTree() override;

    std::string getName() const override { return merging_params.getModeName() + "MergeTree"; }

    bool supportsParallelInsert() const override { return true; }

    bool supportsIndexForIn() const override { return true; }

    bool supportsTransactions() const override { return true; }

    void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    std::optional<UInt64> totalRows(const Settings &) const override;
    std::optional<UInt64> totalRowsByPartitionPredicate(const SelectQueryInfo &, ContextPtr) const override;
    std::optional<UInt64> totalBytes(const Settings &) const override;

    SinkToStoragePtr write(const ASTPtr & query, const StorageMetadataPtr & /*metadata_snapshot*/, ContextPtr context) override;

    /** Perform the next step in combining the parts.
      */
    bool optimize(
        const ASTPtr & query,
        const StorageMetadataPtr & /*metadata_snapshot*/,
        const ASTPtr & partition,
        bool final,
        bool deduplicate,
        const Names & deduplicate_by_columns,
        bool cleanup,
        ContextPtr context) override;

    void mutate(const MutationCommands & commands, ContextPtr context) override;

    bool hasLightweightDeletedMask() const override;

    /// Return introspection information about currently processing or recently processed mutations.
    std::vector<MergeTreeMutationStatus> getMutationsStatus() const override;

    CancellationCode killMutation(const String & mutation_id) override;

    /// Makes backup entries to backup the data of the storage.
    void backupData(BackupEntriesCollector & backup_entries_collector, const String & data_path_in_backup, const std::optional<ASTs> & partitions) override;

    void drop() override;
    void truncate(const ASTPtr &, const StorageMetadataPtr &, ContextPtr, TableExclusiveLockHolder &) override;

    void alter(const AlterCommands & commands, ContextPtr context, AlterLockHolder & table_lock_holder) override;

    void checkTableCanBeDropped() const override;

    ActionLock getActionLock(StorageActionBlockType action_type) override;

    void onActionLockRemove(StorageActionBlockType action_type) override;

    CheckResults checkData(const ASTPtr & query, ContextPtr context) override;

    bool scheduleDataProcessingJob(BackgroundJobsAssignee & assignee) override;

    MergeTreeDeduplicationLog * getDeduplicationLog() { return deduplication_log.get(); }

private:

    /// Mutex and condvar for synchronous mutations wait
    std::mutex mutation_wait_mutex;
    std::condition_variable mutation_wait_event;

    MergeTreeDataSelectExecutor reader;
    MergeTreeDataWriter writer;
    MergeTreeDataMergerMutator merger_mutator;

    std::unique_ptr<MergeTreeDeduplicationLog> deduplication_log;

    /// For block numbers.
    SimpleIncrement increment;

    /// For clearOldParts
    AtomicStopwatch time_after_previous_cleanup_parts;
    /// For clearOldTemporaryDirectories.
    AtomicStopwatch time_after_previous_cleanup_temporary_directories;
    /// For clearOldBrokenDetachedParts
    AtomicStopwatch time_after_previous_cleanup_broken_detached_parts;

    /// Mutex for parts currently processing in background
    /// merging (also with TTL), mutating or moving.
    mutable std::mutex currently_processing_in_background_mutex;
    mutable std::condition_variable currently_processing_in_background_condition;

    /// Parts that currently participate in merge or mutation.
    /// This set have to be used with `currently_processing_in_background_mutex`.
    DataParts currently_merging_mutating_parts;

    std::map<UInt64, MergeTreeMutationEntry> current_mutations_by_version;

    std::atomic<bool> shutdown_called {false};
    std::atomic<bool> flush_called {false};

    void loadMutations();

    /// Load and initialize deduplication logs. Even if deduplication setting
    /// equals zero creates object with deduplication window equals zero.
    void loadDeduplicationLog();

    /** Determines what parts should be merged and merges it.
      * If aggressive - when selects parts don't takes into account their ratio size and novelty (used for OPTIMIZE query).
      * Returns true if merge is finished successfully.
      */
    bool merge(
            bool aggressive,
            const String & partition_id,
            bool final, bool deduplicate,
            const Names & deduplicate_by_columns,
            bool cleanup,
            const MergeTreeTransactionPtr & txn,
            String * out_disable_reason = nullptr,
            bool optimize_skip_merged_partitions = false);

    void renameAndCommitEmptyParts(MutableDataPartsVector & new_parts, Transaction & transaction);

    /// Make part state outdated and queue it to remove without timeout
    /// If force, then stop merges and block them until part state became outdated. Throw exception if part doesn't exists
    /// If not force, then take merges selector and check that part is not participating in background operations.
    MergeTreeDataPartPtr outdatePart(MergeTreeTransaction * txn, const String & part_name, bool force, bool clear_without_timeout = true);
    ActionLock stopMergesAndWait();

    /// Allocate block number for new mutation, write mutation to disk
    /// and into in-memory structures. Wake up merge-mutation task.
    Int64 startMutation(const MutationCommands & commands, ContextPtr query_context);
    /// Wait until mutation with version will finish mutation for all parts
    void waitForMutation(Int64 version);
    void waitForMutation(const String & mutation_id) override;
    void waitForMutation(Int64 version, const String & mutation_id);
    void setMutationCSN(const String & mutation_id, CSN csn) override;


    friend struct CurrentlyMergingPartsTagger;

    MergeMutateSelectedEntryPtr selectPartsToMerge(
        const StorageMetadataPtr & metadata_snapshot,
        bool aggressive,
        const String & partition_id,
        bool final,
        String * disable_reason,
        TableLockHolder & table_lock_holder,
        std::unique_lock<std::mutex> & lock,
        const MergeTreeTransactionPtr & txn,
        bool optimize_skip_merged_partitions = false,
        SelectPartsDecision * select_decision_out = nullptr);


    MergeMutateSelectedEntryPtr selectPartsToMutate(
        const StorageMetadataPtr & metadata_snapshot, String * disable_reason,
        TableLockHolder & table_lock_holder, std::unique_lock<std::mutex> & currently_processing_in_background_mutex_lock);

    /// For current mutations queue, returns maximum version of mutation for a part,
    /// with respect of mutations which would not change it.
    /// Returns 0 if there is no such mutation in active status.
    UInt64 getCurrentMutationVersion(
        const DataPartPtr & part,
        std::unique_lock<std::mutex> & /* currently_processing_in_background_mutex_lock */) const;

    /// Returns the maximum level of all outdated parts in a range (left; right), or 0 in case if empty range.
    /// Merges have to be aware of the outdated part's levels inside designated merge range.
    /// When two parts all_1_1_0, all_3_3_0 are merged into all_1_3_1, the gap between those parts have to be verified.
    /// There should not be an unactive part all_1_1_1. Otherwise it is impossible to load parts after restart, they intersects.
    /// Therefore this function is used in merge predicate in order to prevent merges over the gaps with high level outdated parts.
    UInt32 getMaxLevelInBetween(
        const DataPartPtr & left,
        const DataPartPtr & right) const;

    size_t clearOldMutations(bool truncate = false);

    // Partition helpers
    void dropPartNoWaitNoThrow(const String & part_name) override;
    void dropPart(const String & part_name, bool detach, ContextPtr context) override;
    void dropPartition(const ASTPtr & partition, bool detach, ContextPtr context) override;
    PartitionCommandsResultInfo attachPartition(const ASTPtr & partition, const StorageMetadataPtr & metadata_snapshot, bool part, ContextPtr context) override;

    void replacePartitionFrom(const StoragePtr & source_table, const ASTPtr & partition, bool replace, ContextPtr context) override;
    void movePartitionToTable(const StoragePtr & dest_table, const ASTPtr & partition, ContextPtr context) override;
    bool partIsAssignedToBackgroundOperation(const DataPartPtr & part) const override;
    /// Update mutation entries after part mutation execution. May reset old
    /// errors if mutation was successful. Otherwise update last_failed* fields
    /// in mutation entries.
    void updateMutationEntriesErrors(FutureMergedMutatedPartPtr result_part, bool is_successful, const String & exception_message);

    /// Return empty optional if mutation was killed. Otherwise return partially
    /// filled mutation status with information about error (latest_fail*) and
    /// is_done. mutation_ids filled with mutations with the same errors,
    /// because we can execute several mutations at once. Order is important for
    /// better readability of exception message. If mutation was killed doesn't
    /// return any ids.
    std::optional<MergeTreeMutationStatus> getIncompleteMutationsStatus(Int64 mutation_version, std::set<String> * mutation_ids = nullptr) const;

    void fillNewPartName(MutableDataPartPtr & part, DataPartsLock & lock);

    void startBackgroundMovesIfNeeded() override;

    BackupEntries backupMutations(UInt64 version, const String & data_path_in_backup) const;

    /// Attaches restored parts to the storage.
    void attachRestoredParts(MutableDataPartsVector && parts) override;

    std::unique_ptr<MergeTreeSettings> getDefaultSettings() const override;

    friend class MergeTreeSink;
    friend class MergeTreeData;
    friend class MergePlainMergeTreeTask;
    friend class MutatePlainMergeTreeTask;


protected:

    std::map<int64_t, MutationCommands> getAlterMutationCommandsForPart(const DataPartPtr & part) const override;
};

}

R"MONERO_SOLO_SQL(
CREATE TABLE schema_meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
) STRICT;

INSERT INTO schema_meta(key, value) VALUES ('schema_version', '2');

CREATE TABLE server_sessions (
    id INTEGER PRIMARY KEY,
    public_id BLOB NOT NULL UNIQUE CHECK(length(public_id) = 16),
    started_unix_us INTEGER NOT NULL,
    stopped_unix_us INTEGER,
    version TEXT NOT NULL,
    verifier_commit TEXT,
    clean_shutdown INTEGER NOT NULL DEFAULT 0 CHECK(clean_shutdown IN (0, 1))
) STRICT;

CREATE TABLE workers (
    id INTEGER PRIMARY KEY,
    login TEXT NOT NULL,
    rigid TEXT NOT NULL DEFAULT '',
    first_seen_unix_us INTEGER NOT NULL,
    last_seen_unix_us INTEGER NOT NULL,
    UNIQUE(login, rigid)
) STRICT;

CREATE TABLE connections (
    id INTEGER PRIMARY KEY,
    public_id BLOB NOT NULL UNIQUE CHECK(length(public_id) = 16),
    session_id INTEGER NOT NULL REFERENCES server_sessions(id),
    worker_id INTEGER REFERENCES workers(id),
    peer_family INTEGER NOT NULL,
    peer_address BLOB NOT NULL,
    peer_port INTEGER NOT NULL,
    listen_address TEXT NOT NULL,
    agent TEXT NOT NULL DEFAULT '',
    opened_unix_us INTEGER NOT NULL,
    authenticated_unix_us INTEGER,
    closed_unix_us INTEGER,
    close_reason TEXT,
    last_sent_height INTEGER NOT NULL DEFAULT 0,
    rx_bytes INTEGER NOT NULL DEFAULT 0,
    tx_bytes INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE INDEX connections_worker_time
    ON connections(worker_id, opened_unix_us);
CREATE INDEX connections_peer_time
    ON connections(peer_family, peer_address, opened_unix_us);

CREATE TABLE public_templates (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES server_sessions(id),
    generation INTEGER NOT NULL,
    height INTEGER NOT NULL CHECK(height > 0),
    prev_hash BLOB NOT NULL CHECK(length(prev_hash) = 32),
    seed_hash BLOB NOT NULL CHECK(length(seed_hash) = 32),
    next_seed_hash BLOB CHECK(next_seed_hash IS NULL OR length(next_seed_hash) = 32),
    difficulty_dec TEXT NOT NULL,
    wide_difficulty_hex TEXT,
    reserved_offset INTEGER NOT NULL,
    reserve_size INTEGER NOT NULL CHECK(reserve_size = 16),
    blocktemplate_blob BLOB NOT NULL,
    blockhashing_blob BLOB NOT NULL,
    fetched_unix_us INTEGER NOT NULL,
    fetch_reason TEXT NOT NULL,
    UNIQUE(session_id, generation)
) STRICT;

CREATE INDEX public_templates_height
    ON public_templates(height, id);

CREATE TABLE private_jobs (
    id INTEGER PRIMARY KEY,
    public_job_id BLOB NOT NULL UNIQUE CHECK(length(public_job_id) = 16),
    connection_id INTEGER NOT NULL REFERENCES connections(id),
    template_id INTEGER NOT NULL REFERENCES public_templates(id),
    height INTEGER NOT NULL,
    entropy BLOB NOT NULL UNIQUE CHECK(length(entropy) = 16),
    seed_hash BLOB NOT NULL CHECK(length(seed_hash) = 32),
    mspv_seed_id_dec TEXT,
    assigned_difficulty_dec TEXT NOT NULL,
    target64_le BLOB NOT NULL CHECK(length(target64_le) = 8),
    network_difficulty_dec TEXT NOT NULL,
    nonce_offset INTEGER NOT NULL,
    nonce_size INTEGER NOT NULL CHECK(nonce_size = 4),
    reserved_offset INTEGER NOT NULL,
    reserved_size INTEGER NOT NULL CHECK(reserved_size = 16),
    private_block_blob BLOB NOT NULL,
    hashing_blob BLOB NOT NULL,
    created_unix_us INTEGER NOT NULL,
    queued_unix_us INTEGER,
    expires_unix_us INTEGER NOT NULL,
    retired_unix_us INTEGER
) STRICT;

CREATE INDEX private_jobs_connection_time
    ON private_jobs(connection_id, created_unix_us DESC);
CREATE INDEX private_jobs_height
    ON private_jobs(height, id);

CREATE TABLE shares (
    id INTEGER PRIMARY KEY,
    round_id INTEGER NOT NULL REFERENCES rounds(id),
    connection_id INTEGER NOT NULL REFERENCES connections(id),
    worker_id INTEGER REFERENCES workers(id),
    job_id INTEGER REFERENCES private_jobs(id),
    request_sequence INTEGER NOT NULL CHECK(request_sequence >= 1),
    miner_request_id_type TEXT CHECK(
        miner_request_id_type IS NULL OR miner_request_id_type IN ('integer', 'string')
    ),
    miner_request_id_text TEXT,
    received_unix_us INTEGER NOT NULL,
    completed_unix_us INTEGER,
    nonce BLOB CHECK(nonce IS NULL OR length(nonce) = 4),
    assigned_difficulty_dec TEXT,
    actual_difficulty_dec TEXT,
    network_difficulty_dec TEXT,
    height_is_older INTEGER NOT NULL DEFAULT 0 CHECK(height_is_older IN (0, 1)),
    claimed_candidate INTEGER NOT NULL DEFAULT 0 CHECK(claimed_candidate IN (0, 1)),
    candidate_admission TEXT NOT NULL DEFAULT 'not_candidate' CHECK(
        candidate_admission IN (
            'not_candidate', 'admitted', 'deferred', 'existing',
            'trusted_rate_limited'
        )
    ),
    status TEXT NOT NULL CHECK(status IN (
        'received', 'verifying', 'accepted', 'stale', 'duplicate',
        'low_difficulty', 'invalid_result', 'unknown_job', 'malformed',
        'unauthenticated', 'server_busy', 'verifier_failed', 'cancelled'
    )),
    error_code TEXT,
    error_message TEXT,
    provenance TEXT NOT NULL CHECK(provenance IN ('verified', 'claimed', 'pending')),
    credited_difficulty_dec TEXT,
    verifier_ticket_dec TEXT,
    verifier_seed_id_dec TEXT,
    verifier_queue_ns INTEGER,
    verifier_hash_ns INTEGER,
    verifier_total_ns INTEGER,
    candidate_id INTEGER REFERENCES candidates(id),
    CHECK(
        (miner_request_id_type IS NULL AND miner_request_id_text IS NULL) OR
        (miner_request_id_type IS NOT NULL AND miner_request_id_text IS NOT NULL)
    ),
    UNIQUE(connection_id, request_sequence)
) STRICT;

CREATE INDEX shares_time ON shares(received_unix_us, id);
CREATE INDEX shares_worker_time ON shares(worker_id, received_unix_us, id);
CREATE INDEX shares_status_time ON shares(status, received_unix_us, id);
CREATE INDEX shares_round_status ON shares(round_id, status, id);
CREATE INDEX shares_accepted_actual_difficulty_rank
    ON shares(length(actual_difficulty_dec) DESC, actual_difficulty_dec DESC, id)
    WHERE status = 'accepted' AND actual_difficulty_dec IS NOT NULL;

CREATE TRIGGER share_round_is_immutable
BEFORE UPDATE OF round_id ON shares
WHEN NEW.round_id != OLD.round_id
BEGIN
    SELECT RAISE(ABORT, 'share round is immutable');
END;

CREATE TABLE share_hashes (
    share_id INTEGER NOT NULL REFERENCES shares(id) ON DELETE CASCADE,
    role TEXT NOT NULL CHECK(role IN ('claimed', 'computed')),
    hash BLOB NOT NULL CHECK(length(hash) = 32),
    meets_share_target INTEGER CHECK(meets_share_target IN (0, 1)),
    meets_network_target INTEGER CHECK(meets_network_target IN (0, 1)),
    PRIMARY KEY(share_id, role)
) WITHOUT ROWID, STRICT;

CREATE TABLE duplicate_keys (
    key BLOB PRIMARY KEY CHECK(length(key) = 48),
    height INTEGER NOT NULL,
    first_share_id INTEGER NOT NULL REFERENCES shares(id),
    role TEXT NOT NULL CHECK(role IN ('claimed', 'computed', 'both')),
    active INTEGER NOT NULL CHECK(active IN (0, 1)),
    reserved_unix_us INTEGER NOT NULL,
    retired_unix_us INTEGER,
    generation_token INTEGER NOT NULL
) WITHOUT ROWID, STRICT;

CREATE INDEX duplicate_keys_active_height
    ON duplicate_keys(active, height);

CREATE TABLE candidates (
    id INTEGER PRIMARY KEY,
    candidate_key BLOB NOT NULL UNIQUE CHECK(length(candidate_key) = 32),
    first_share_id INTEGER NOT NULL REFERENCES shares(id),
    job_id INTEGER NOT NULL REFERENCES private_jobs(id),
    connection_id INTEGER NOT NULL REFERENCES connections(id),
    height INTEGER NOT NULL,
    peer_family INTEGER NOT NULL,
    peer_address BLOB NOT NULL,
    frozen_block_blob BLOB NOT NULL,
    miner_tx_hash BLOB NOT NULL CHECK(length(miner_tx_hash) = 32),
    expected_block_id BLOB CHECK(expected_block_id IS NULL OR length(expected_block_id) = 32),
    canonical_block_id BLOB CHECK(canonical_block_id IS NULL OR length(canonical_block_id) = 32),
    state TEXT NOT NULL CHECK(state IN (
        'journaled', 'dispatching', 'retry_wait', 'accepted',
        'rejected', 'ambiguous', 'accepted_by_reconciliation'
    )),
    attempt_count INTEGER NOT NULL DEFAULT 0,
    max_attempts INTEGER NOT NULL CHECK(max_attempts BETWEEN 1 AND 4),
    had_indeterminate INTEGER NOT NULL DEFAULT 0 CHECK(had_indeterminate IN (0, 1)),
    reconciliation_cycle_count INTEGER NOT NULL DEFAULT 0,
    next_reconciliation_unix_us INTEGER,
    reconciliation_exhausted_unix_us INTEGER,
    created_unix_us INTEGER NOT NULL,
    updated_unix_us INTEGER NOT NULL,
    accepted_unix_us INTEGER,
    terminal_reason TEXT
) STRICT;

CREATE INDEX candidates_state_time ON candidates(state, updated_unix_us, id);
CREATE INDEX candidates_miner_tx ON candidates(miner_tx_hash);

CREATE TABLE candidate_attempts (
    id INTEGER PRIMARY KEY,
    candidate_id INTEGER NOT NULL REFERENCES candidates(id),
    attempt_number INTEGER NOT NULL CHECK(attempt_number >= 1),
    rpc_request_id INTEGER NOT NULL,
    started_unix_us INTEGER NOT NULL,
    completed_unix_us INTEGER,
    classification TEXT NOT NULL CHECK(classification IN (
        'dispatching', 'accepted', 'explicit_rejection', 'indeterminate'
    )),
    http_status INTEGER,
    rpc_error_code INTEGER,
    daemon_status TEXT,
    daemon_block_id BLOB CHECK(daemon_block_id IS NULL OR length(daemon_block_id) = 32),
    response_excerpt TEXT,
    UNIQUE(candidate_id, attempt_number)
) STRICT;

CREATE TRIGGER candidate_attempt_within_snapshot
BEFORE INSERT ON candidate_attempts
WHEN NEW.attempt_number > (
    SELECT max_attempts FROM candidates WHERE id = NEW.candidate_id
)
BEGIN
    SELECT RAISE(ABORT, 'candidate attempt exceeds snapshotted maximum');
END;

CREATE TABLE candidate_reconciliations (
    id INTEGER PRIMARY KEY,
    candidate_id INTEGER NOT NULL REFERENCES candidates(id),
    cycle_number INTEGER NOT NULL CHECK(cycle_number >= 1),
    lookup_kind TEXT NOT NULL CHECK(lookup_kind IN ('expected_hash', 'height')),
    rpc_request_id INTEGER NOT NULL,
    requested_block_id BLOB CHECK(requested_block_id IS NULL OR length(requested_block_id) = 32),
    started_unix_us INTEGER NOT NULL,
    completed_unix_us INTEGER,
    classification TEXT NOT NULL CHECK(classification IN (
        'querying', 'positive', 'inconclusive', 'indeterminate'
    )),
    observed_block_id BLOB CHECK(observed_block_id IS NULL OR length(observed_block_id) = 32),
    observed_height INTEGER,
    observed_miner_tx_hash BLOB CHECK(
        observed_miner_tx_hash IS NULL OR length(observed_miner_tx_hash) = 32
    ),
    observed_orphan INTEGER CHECK(observed_orphan IS NULL OR observed_orphan IN (0, 1)),
    response_excerpt TEXT,
    UNIQUE(candidate_id, cycle_number, lookup_kind)
) STRICT;

CREATE INDEX candidate_reconciliations_candidate
    ON candidate_reconciliations(candidate_id, cycle_number, id);

CREATE TABLE rounds (
    id INTEGER PRIMARY KEY,
    opened_unix_us INTEGER NOT NULL,
    closed_unix_us INTEGER,
    state TEXT NOT NULL CHECK(state IN ('open', 'closed')),
    accepted_candidate_id INTEGER UNIQUE REFERENCES candidates(id),
    accepted_height INTEGER,
    miner_tx_hash BLOB CHECK(miner_tx_hash IS NULL OR length(miner_tx_hash) = 32),
    block_id BLOB CHECK(block_id IS NULL OR length(block_id) = 32),
    credited_difficulty_dec TEXT NOT NULL DEFAULT '0',
    accepted_share_count INTEGER NOT NULL DEFAULT 0 CHECK(accepted_share_count >= 0),
    effort_finalized_unix_us INTEGER,
    finalized_effort_segment_count INTEGER CHECK(
        finalized_effort_segment_count IS NULL OR
        finalized_effort_segment_count >= 0
    ),
    CHECK(
        (state = 'open' AND closed_unix_us IS NULL AND
         accepted_candidate_id IS NULL AND accepted_height IS NULL AND
         miner_tx_hash IS NULL AND block_id IS NULL AND
         effort_finalized_unix_us IS NULL AND
         finalized_effort_segment_count IS NULL) OR
        (state = 'closed' AND closed_unix_us IS NOT NULL AND
         closed_unix_us >= opened_unix_us AND
         accepted_candidate_id IS NOT NULL AND accepted_height IS NOT NULL AND
         miner_tx_hash IS NOT NULL AND
         ((effort_finalized_unix_us IS NULL AND
           finalized_effort_segment_count IS NULL) OR
          (effort_finalized_unix_us >= closed_unix_us AND
           finalized_effort_segment_count IS NOT NULL)))
    )
) STRICT;

CREATE UNIQUE INDEX exactly_one_open_round
    ON rounds(state) WHERE state = 'open';

CREATE TABLE round_work_segments (
    round_id INTEGER NOT NULL REFERENCES rounds(id),
    source TEXT NOT NULL CHECK(source IN ('verified', 'claimed')),
    network_difficulty_dec TEXT NOT NULL CHECK(network_difficulty_dec != '0'),
    credited_difficulty_dec TEXT NOT NULL CHECK(credited_difficulty_dec != '0'),
    accepted_share_count INTEGER NOT NULL CHECK(accepted_share_count > 0),
    PRIMARY KEY(round_id, source, network_difficulty_dec),
    CHECK(length(network_difficulty_dec) > 0),
    CHECK(length(credited_difficulty_dec) > 0),
    CHECK(network_difficulty_dec NOT GLOB '*[^0-9]*'),
    CHECK(credited_difficulty_dec NOT GLOB '*[^0-9]*'),
    CHECK(length(network_difficulty_dec) = 1 OR
          substr(network_difficulty_dec, 1, 1) != '0'),
    CHECK(length(credited_difficulty_dec) = 1 OR
          substr(credited_difficulty_dec, 1, 1) != '0')
) WITHOUT ROWID, STRICT;

CREATE TRIGGER round_work_segment_insert_before_finalization
BEFORE INSERT ON round_work_segments
WHEN (SELECT effort_finalized_unix_us FROM rounds WHERE id = NEW.round_id)
     IS NOT NULL
BEGIN
    SELECT RAISE(ABORT, 'finalized round effort is immutable');
END;

CREATE TRIGGER round_work_segment_update_before_finalization
BEFORE UPDATE ON round_work_segments
WHEN (SELECT effort_finalized_unix_us FROM rounds WHERE id = OLD.round_id)
         IS NOT NULL OR
     (SELECT effort_finalized_unix_us FROM rounds WHERE id = NEW.round_id)
         IS NOT NULL
BEGIN
    SELECT RAISE(ABORT, 'finalized round effort is immutable');
END;

CREATE TRIGGER round_work_segment_delete_before_finalization
BEFORE DELETE ON round_work_segments
WHEN (SELECT effort_finalized_unix_us FROM rounds WHERE id = OLD.round_id)
     IS NOT NULL
BEGIN
    SELECT RAISE(ABORT, 'finalized round effort is immutable');
END;

CREATE TRIGGER round_effort_finalization_is_consistent
BEFORE UPDATE OF effort_finalized_unix_us, finalized_effort_segment_count ON rounds
WHEN OLD.effort_finalized_unix_us IS NULL AND
     NEW.effort_finalized_unix_us IS NOT NULL AND
     (NEW.state != 'closed' OR
      EXISTS (
          SELECT 1 FROM shares
          WHERE round_id = NEW.id AND status IN ('received', 'verifying')
      ) OR
      NEW.finalized_effort_segment_count != (
          SELECT count(*) FROM round_work_segments WHERE round_id = NEW.id
      ) OR
      NEW.accepted_share_count != coalesce((
          SELECT sum(accepted_share_count)
          FROM round_work_segments WHERE round_id = NEW.id
      ), 0))
BEGIN
    SELECT RAISE(ABORT, 'round effort cannot be finalized');
END;

CREATE TRIGGER finalized_round_is_immutable
BEFORE UPDATE ON rounds
WHEN OLD.effort_finalized_unix_us IS NOT NULL AND
     (NEW.opened_unix_us IS NOT OLD.opened_unix_us OR
      NEW.closed_unix_us IS NOT OLD.closed_unix_us OR
      NEW.state IS NOT OLD.state OR
      NEW.accepted_candidate_id IS NOT OLD.accepted_candidate_id OR
      NEW.accepted_height IS NOT OLD.accepted_height OR
      NEW.miner_tx_hash IS NOT OLD.miner_tx_hash OR
      NEW.block_id IS NOT OLD.block_id OR
      NEW.credited_difficulty_dec IS NOT OLD.credited_difficulty_dec OR
      NEW.accepted_share_count IS NOT OLD.accepted_share_count OR
      NEW.effort_finalized_unix_us IS NOT OLD.effort_finalized_unix_us OR
      NEW.finalized_effort_segment_count IS NOT
          OLD.finalized_effort_segment_count)
BEGIN
    SELECT RAISE(ABORT, 'finalized round is immutable');
END;

CREATE TABLE hashrate_buckets (
    scope_type TEXT NOT NULL CHECK(scope_type IN ('global', 'connection', 'worker')),
    scope_id INTEGER NOT NULL,
    second_utc INTEGER NOT NULL,
    credited_difficulty_dec TEXT NOT NULL,
    accepted_shares INTEGER NOT NULL,
    source TEXT NOT NULL CHECK(source IN ('verified', 'claimed')),
    PRIMARY KEY(scope_type, scope_id, second_utc, source),
    CHECK(
        (scope_type = 'global' AND scope_id = 0) OR
        (scope_type IN ('connection', 'worker') AND scope_id > 0)
    )
) WITHOUT ROWID, STRICT;

CREATE INDEX hashrate_buckets_time ON hashrate_buckets(second_utc);

CREATE TABLE bans (
    id INTEGER PRIMARY KEY,
    peer_family INTEGER NOT NULL,
    peer_address BLOB NOT NULL,
    created_unix_us INTEGER NOT NULL,
    expires_unix_us INTEGER NOT NULL,
    evidence_window_started_unix_us INTEGER NOT NULL,
    evidence_window_ended_unix_us INTEGER NOT NULL,
    reason TEXT NOT NULL,
    active INTEGER NOT NULL CHECK(active IN (0, 1))
) STRICT;

CREATE INDEX bans_expiry ON bans(active, expires_unix_us);
CREATE UNIQUE INDEX one_active_ban_per_peer
    ON bans(peer_family, peer_address) WHERE active = 1;

CREATE TABLE abuse_events (
    id INTEGER PRIMARY KEY,
    connection_id INTEGER REFERENCES connections(id),
    share_id INTEGER REFERENCES shares(id),
    candidate_id INTEGER REFERENCES candidates(id),
    peer_family INTEGER NOT NULL,
    peer_address BLOB NOT NULL,
    kind TEXT NOT NULL,
    weight INTEGER NOT NULL,
    created_unix_us INTEGER NOT NULL,
    detail TEXT
) STRICT;

CREATE INDEX abuse_peer_time
    ON abuse_events(peer_family, peer_address, created_unix_us);

CREATE UNIQUE INDEX one_candidate_abuse_event_per_kind
    ON abuse_events(candidate_id, kind)
    WHERE candidate_id IS NOT NULL AND kind IN (
        'verified_false_candidate', 'candidate_mismatch',
        'trusted_candidate_rejection'
    );

CREATE TABLE candidate_verdicts (
    share_id INTEGER NOT NULL REFERENCES shares(id),
    kind TEXT NOT NULL CHECK(kind IN ('false_candidate', 'candidate_mismatch')),
    candidate_key BLOB NOT NULL CHECK(length(candidate_key) = 32),
    candidate_id INTEGER REFERENCES candidates(id),
    disposition TEXT NOT NULL CHECK(
        disposition IN ('pending', 'actionable', 'suppressed')
    ),
    created_unix_us INTEGER NOT NULL,
    resolved_unix_us INTEGER,
    abuse_event_id INTEGER UNIQUE REFERENCES abuse_events(id),
    PRIMARY KEY(share_id, kind),
    CHECK(
        (disposition = 'pending' AND resolved_unix_us IS NULL AND
         abuse_event_id IS NULL) OR
        (disposition = 'actionable' AND resolved_unix_us IS NOT NULL AND
         abuse_event_id IS NOT NULL) OR
        (disposition = 'suppressed' AND resolved_unix_us IS NOT NULL AND
         abuse_event_id IS NULL)
    )
) WITHOUT ROWID, STRICT;

CREATE INDEX candidate_verdicts_candidate
    ON candidate_verdicts(candidate_id, disposition, share_id);
CREATE INDEX candidate_verdicts_key
    ON candidate_verdicts(candidate_key, disposition, share_id);

CREATE TRIGGER candidate_verdict_key_matches_on_insert
BEFORE INSERT ON candidate_verdicts
WHEN NEW.candidate_id IS NOT NULL AND NOT EXISTS (
    SELECT 1 FROM candidates
    WHERE id = NEW.candidate_id AND candidate_key = NEW.candidate_key
)
BEGIN
    SELECT RAISE(ABORT, 'candidate verdict key does not match candidate');
END;

CREATE TRIGGER candidate_verdict_key_matches_on_update
BEFORE UPDATE OF candidate_id, candidate_key ON candidate_verdicts
WHEN NEW.candidate_id IS NOT NULL AND NOT EXISTS (
    SELECT 1 FROM candidates
    WHERE id = NEW.candidate_id AND candidate_key = NEW.candidate_key
)
BEGIN
    SELECT RAISE(ABORT, 'candidate verdict key does not match candidate');
END;

CREATE TABLE ban_abuse_events (
    ban_id INTEGER NOT NULL REFERENCES bans(id) ON DELETE CASCADE,
    abuse_event_id INTEGER NOT NULL REFERENCES abuse_events(id),
    PRIMARY KEY(ban_id, abuse_event_id)
) WITHOUT ROWID, STRICT;

CREATE INDEX ban_abuse_events_event
    ON ban_abuse_events(abuse_event_id, ban_id);

CREATE TABLE events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES server_sessions(id),
    created_unix_us INTEGER NOT NULL,
    type TEXT NOT NULL,
    connection_id INTEGER REFERENCES connections(id),
    worker_id INTEGER REFERENCES workers(id),
    template_id INTEGER REFERENCES public_templates(id),
    job_id INTEGER REFERENCES private_jobs(id),
    share_id INTEGER REFERENCES shares(id),
    candidate_id INTEGER REFERENCES candidates(id),
    round_id INTEGER REFERENCES rounds(id),
    payload_json TEXT NOT NULL
) STRICT;

CREATE INDEX events_time ON events(created_unix_us, id);
CREATE INDEX events_type_id ON events(type, id);
CREATE INDEX events_share_result_share ON events(share_id, id)
    WHERE type = 'share_result' AND share_id IS NOT NULL;
CREATE INDEX events_share_result_round_share ON events(round_id, share_id, id)
    WHERE type = 'share_result' AND round_id IS NOT NULL AND share_id IS NOT NULL;

CREATE TABLE blocknotify_deliveries (
    id INTEGER PRIMARY KEY,
    candidate_id INTEGER NOT NULL UNIQUE REFERENCES candidates(id),
    miner_tx_hash BLOB NOT NULL CHECK(length(miner_tx_hash) = 32),
    state TEXT NOT NULL CHECK(state IN ('pending', 'running', 'delivered', 'retry_wait')),
    attempt_count INTEGER NOT NULL DEFAULT 0,
    next_attempt_unix_us INTEGER,
    started_unix_us INTEGER,
    completed_unix_us INTEGER,
    exit_code INTEGER,
    term_signal INTEGER,
    stderr_excerpt TEXT,
    last_error TEXT
) STRICT;
)MONERO_SOLO_SQL"

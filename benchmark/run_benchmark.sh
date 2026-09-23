set -u

RUNS="${RUNS:-30}"
PORT="${PORT:-9000}"
RECEIVER_IP="${RECEIVER_IP:-127.0.0.1}"
FILE="${FILE:-bench_10MiB.bin}"
OUTPUT_DIR="${OUTPUT_DIR:-benchmark/results}"
CSV_FILE="${CSV_FILE:-${OUTPUT_DIR}/results.csv}"
LOG_DIR="${LOG_DIR:-${OUTPUT_DIR}/logs}"
SENDER="${SENDER:-./sender}"
RECEIVER="${RECEIVER:-./receiver}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-60}"

mkdir -p "$OUTPUT_DIR" "$LOG_DIR"

if ! command -v timeout >/dev/null 2>&1; then
    echo "error: 'timeout' command is required" >&2
    exit 1
fi

if [[ ! -x "$SENDER" ]]; then
    echo "error: sender binary not found or not executable: $SENDER" >&2
    exit 1
fi

if [[ ! -x "$RECEIVER" ]]; then
    echo "error: receiver binary not found or not executable: $RECEIVER" >&2
    exit 1
fi

if [[ ! -f "$FILE" ]]; then
    echo "Creating benchmark file: $FILE"

    if ! dd if=/dev/urandom of="$FILE" bs=1M count=10 status=none; then
        echo "error: failed to create benchmark file" >&2
        exit 1
    fi
fi

cat > "$CSV_FILE" <<'CSV'
run,status,payload_bytes,transfer_time_ms,goodput_mbps,initial_data_sends,retransmissions,retransmission_rate,acks_received,rtt_samples,rtt_p50_us,rtt_p95_us,rtt_p99_us
CSV

receiver_pid=""

cleanup() {
    if [[ -n "$receiver_pid" ]] && kill -0 "$receiver_pid" 2>/dev/null; then
        kill "$receiver_pid" 2>/dev/null || true
        wait "$receiver_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

extract_metric() {
    local key="$1"
    local log="$2"

    grep -m1 "^${key}=" "$log" 2>/dev/null | cut -d= -f2-
}

for ((run=1; run<=RUNS; run++)); do
    run_dir="${OUTPUT_DIR}/run_${run}"
    sender_log="${LOG_DIR}/sender_${run}.log"
    receiver_log="${LOG_DIR}/receiver_${run}.log"

    rm -rf "$run_dir"
    mkdir -p "$run_dir"

    echo "[${run}/${RUNS}] starting receiver"

    "$RECEIVER" "$PORT" "$run_dir" >"$receiver_log" 2>&1 &
    receiver_pid=$!

    # Give the receiver a moment to bind before starting the sender.
    sleep 0.1

    echo "[${run}/${RUNS}] running transfer"

    if timeout "${TIMEOUT_SECONDS}s" \
        "$SENDER" "$RECEIVER_IP" "$PORT" "$FILE" \
        >"$sender_log" 2>&1; then
        status="success"
    else
        status="failure"
    fi

    # Give the receiver a short opportunity to exit normally.
    # The receiver exits after its FIN_ACK grace period.
    if [[ -n "$receiver_pid" ]] && kill -0 "$receiver_pid" 2>/dev/null; then
        for ((wait_i=0; wait_i<50; wait_i++)); do
            if ! kill -0 "$receiver_pid" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done

        # Prevent a failed or hung receiver from blocking the benchmark.
        if kill -0 "$receiver_pid" 2>/dev/null; then
            kill "$receiver_pid" 2>/dev/null || true
        fi

        wait "$receiver_pid" 2>/dev/null || true
    fi

    receiver_pid=""

    payload_bytes="$(extract_metric "payload_bytes" "$sender_log")"
    transfer_time_ms="$(extract_metric "transfer_time_ms" "$sender_log")"
    goodput_mbps="$(extract_metric "goodput_mbps" "$sender_log")"
    initial_data_sends="$(extract_metric "initial_data_sends" "$sender_log")"
    retransmissions="$(extract_metric "retransmissions" "$sender_log")"
    retransmission_rate="$(extract_metric "retransmission_rate" "$sender_log")"
    acks_received="$(extract_metric "acks_received" "$sender_log")"
    rtt_samples="$(extract_metric "rtt_samples" "$sender_log")"
    rtt_p50_us="$(extract_metric "rtt_p50_us" "$sender_log")"
    rtt_p95_us="$(extract_metric "rtt_p95_us" "$sender_log")"
    rtt_p99_us="$(extract_metric "rtt_p99_us" "$sender_log")"

    # Keep one CSV row per run, including failed runs.
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$run" \
        "$status" \
        "$payload_bytes" \
        "$transfer_time_ms" \
        "$goodput_mbps" \
        "$initial_data_sends" \
        "$retransmissions" \
        "$retransmission_rate" \
        "$acks_received" \
        "$rtt_samples" \
        "$rtt_p50_us" \
        "$rtt_p95_us" \
        "$rtt_p99_us" \
        >> "$CSV_FILE"

    echo "[${run}/${RUNS}] ${status}"
done

echo
echo "Benchmark complete."
echo "CSV:  $CSV_FILE"
echo "Logs: $LOG_DIR"
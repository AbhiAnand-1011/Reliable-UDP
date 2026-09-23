#!/usr/bin/env python3

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path


REQUIRED_COLUMNS = [
    "run",
    "status",
    "payload_bytes",
    "transfer_time_ms",
    "goodput_mbps",
    "initial_data_sends",
    "retransmissions",
    "retransmission_rate",
    "acks_received",
    "rtt_samples",
    "rtt_p50_us",
    "rtt_p95_us",
    "rtt_p99_us",
]


def percentile(values, p):
    if not values:
        return math.nan

    if not 0.0 <= p <= 100.0:
        raise ValueError("percentile must be between 0 and 100")

    data = sorted(float(value) for value in values)

    if len(data) == 1:
        return data[0]

    position = (p / 100.0) * (len(data) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)

    if lower == upper:
        return data[lower]

    fraction = position - lower

    return data[lower] + fraction * (data[upper] - data[lower])


def parse_number(row, column, row_number, kind=float, required=False):
    value = row.get(column, "")

    if value is None:
        value = ""

    value = value.strip()

    if value == "":
        if required:
            raise ValueError(
                f"row {row_number}: required field '{column}' is empty"
            )
        return None

    try:
        return kind(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(
            f"row {row_number}: invalid value in '{column}': {value!r}"
        ) from exc


def mean_or_nan(values):
    return statistics.mean(values) if values else math.nan


def median_or_nan(values):
    return statistics.median(values) if values else math.nan


def fmt(value, digits=3):
    if value is None:
        return "N/A"

    if isinstance(value, float) and math.isnan(value):
        return "N/A"

    return f"{value:.{digits}f}"


def load_results(path):
    try:
        with path.open("r", newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)

            if reader.fieldnames is None:
                raise ValueError("CSV file is missing a header row")

            missing = [
                column
                for column in REQUIRED_COLUMNS
                if column not in reader.fieldnames
            ]

            if missing:
                raise ValueError(
                    "CSV is missing required columns: "
                    + ", ".join(missing)
                )

            rows = list(reader)

    except OSError as exc:
        raise ValueError(f"failed to read '{path}': {exc}") from exc

    if not rows:
        raise ValueError(f"CSV contains no benchmark runs: {path}")

    return rows


def main():
    parser = argparse.ArgumentParser(
        description="Analyze Reliable-UDP benchmark results."
    )

    parser.add_argument(
        "csv_file",
        nargs="?",
        default="benchmark/results/results.csv",
    )

    parser.add_argument(
        "--summary-output",
        default="",
    )

    args = parser.parse_args()
    csv_path = Path(args.csv_file)

    try:
        rows = load_results(csv_path)

        total_runs = len(rows)
        successful_runs = []
        failed_runs = []
        incomplete_success_runs = []

        for row_index, row in enumerate(rows, start=2):
            status = (row.get("status") or "").strip().lower()

            run_number = parse_number(
                row,
                "run",
                row_index,
                kind=int,
                required=True,
            )

            row["_run"] = run_number

            if status != "success":
                failed_runs.append(row)
                continue

            transfer_time = parse_number(
                row,
                "transfer_time_ms",
                row_index,
            )

            goodput = parse_number(
                row,
                "goodput_mbps",
                row_index,
            )

            if transfer_time is None or goodput is None:
                incomplete_success_runs.append(run_number)
                continue

            row["_transfer_time_ms"] = transfer_time
            row["_goodput_mbps"] = goodput

            successful_runs.append(row)

        performance_runs = successful_runs

        success_rate = (
            len(successful_runs) / total_runs * 100.0
            if total_runs > 0
            else math.nan
        )

        print("=== Reliable-UDP Benchmark Summary ===")
        print(f"CSV: {csv_path}")
        print(f"Total runs: {total_runs}")
        print(f"Successful runs: {len(successful_runs)}")
        print(f"Failed runs: {len(failed_runs)}")
        print(
            "Incomplete successful runs: "
            f"{len(incomplete_success_runs)}"
        )
        print(f"Success rate: {fmt(success_rate, 2)}%")
        print()

        if not performance_runs:
            print("No complete successful runs available for analysis.")

            if failed_runs:
                print(
                    "Failed runs: "
                    + ", ".join(
                        str(row["_run"]) for row in failed_runs
                    )
                )

            if incomplete_success_runs:
                print(
                    "Incomplete successful runs: "
                    + ", ".join(
                        str(run) for run in incomplete_success_runs
                    )
                )

            return 0

        transfer_times = []
        goodputs = []
        payload_values = []

        initial_sends = []
        retransmissions = []
        retransmission_rates = []
        ack_counts = []
        rtt_sample_counts = []

        rtt_p50_values = []
        rtt_p95_values = []
        rtt_p99_values = []

        for row_index, row in enumerate(rows, start=2):
            status = (row.get("status") or "").strip().lower()

            if status != "success":
                continue

            if (
                "_transfer_time_ms" not in row
                or "_goodput_mbps" not in row
            ):
                continue

            transfer_times.append(row["_transfer_time_ms"])
            goodputs.append(row["_goodput_mbps"])

            payload = parse_number(
                row,
                "payload_bytes",
                row_index,
                kind=int,
            )

            initial = parse_number(
                row,
                "initial_data_sends",
                row_index,
                kind=int,
            )

            retrans = parse_number(
                row,
                "retransmissions",
                row_index,
                kind=int,
            )

            retrans_rate = parse_number(
                row,
                "retransmission_rate",
                row_index,
            )

            acks = parse_number(
                row,
                "acks_received",
                row_index,
                kind=int,
            )

            rtt_samples = parse_number(
                row,
                "rtt_samples",
                row_index,
                kind=int,
            )

            rtt_p50 = parse_number(
                row,
                "rtt_p50_us",
                row_index,
            )

            rtt_p95 = parse_number(
                row,
                "rtt_p95_us",
                row_index,
            )

            rtt_p99 = parse_number(
                row,
                "rtt_p99_us",
                row_index,
            )

            if payload is not None:
                payload_values.append(payload)

            if initial is not None:
                initial_sends.append(initial)

            if retrans is not None:
                retransmissions.append(retrans)

            if retrans_rate is not None:
                retransmission_rates.append(retrans_rate)

            if acks is not None:
                ack_counts.append(acks)

            if rtt_samples is not None:
                rtt_sample_counts.append(rtt_samples)

            if rtt_p50 is not None:
                rtt_p50_values.append(rtt_p50)

            if rtt_p95 is not None:
                rtt_p95_values.append(rtt_p95)

            if rtt_p99 is not None:
                rtt_p99_values.append(rtt_p99)

        print("--- Transfer Time ---")
        print(f"Mean:   {fmt(mean_or_nan(transfer_times))} ms")
        print(f"Median: {fmt(median_or_nan(transfer_times))} ms")
        print(f"p50:    {fmt(percentile(transfer_times, 50))} ms")
        print(f"p95:    {fmt(percentile(transfer_times, 95))} ms")
        print(f"p99:    {fmt(percentile(transfer_times, 99))} ms")
        print(f"Min:    {fmt(min(transfer_times))} ms")
        print(f"Max:    {fmt(max(transfer_times))} ms")
        print()

        print("--- Goodput ---")
        print(f"Mean:   {fmt(mean_or_nan(goodputs))} Mbps")
        print(f"Median: {fmt(median_or_nan(goodputs))} Mbps")
        print(f"p50:    {fmt(percentile(goodputs, 50))} Mbps")
        print(f"p95:    {fmt(percentile(goodputs, 95))} Mbps")
        print(f"p99:    {fmt(percentile(goodputs, 99))} Mbps")
        print(f"Min:    {fmt(min(goodputs))} Mbps")
        print(f"Max:    {fmt(max(goodputs))} Mbps")
        print()

        total_initial = sum(initial_sends)
        total_retransmissions = sum(retransmissions)
        total_acks = sum(ack_counts)
        total_rtt_samples = sum(rtt_sample_counts)

        if total_initial > 0:
            overall_retransmission_rate = (
                total_retransmissions / total_initial
            )
            ack_ratio = total_acks / total_initial
        else:
            overall_retransmission_rate = math.nan
            ack_ratio = math.nan

        print("--- Reliability ---")
        print(f"Initial DATA sends: {total_initial}")
        print(f"Retransmissions:    {total_retransmissions}")
        print(
            "Overall retransmission rate: "
            f"{fmt(overall_retransmission_rate * 100.0, 4)}%"
        )
        print(
            "Mean per-run retransmission rate: "
            f"{fmt(mean_or_nan(retransmission_rates) * 100.0, 4)}%"
        )
        print(f"ACKs received: {total_acks}")
        print(f"ACK / initial DATA ratio: {fmt(ack_ratio, 4)}")
        print()

        print("--- RTT ---")
        print(f"Total RTT samples: {total_rtt_samples}")

        if rtt_p50_values:
            print(
                "Median per-run RTT p50: "
                f"{fmt(median_or_nan(rtt_p50_values))} us"
            )
        else:
            print("Median per-run RTT p50: N/A")

        if rtt_p95_values:
            print(
                "Median per-run RTT p95: "
                f"{fmt(median_or_nan(rtt_p95_values))} us"
            )
        else:
            print("Median per-run RTT p95: N/A")

        if rtt_p99_values:
            print(
                "Median per-run RTT p99: "
                f"{fmt(median_or_nan(rtt_p99_values))} us"
            )
        else:
            print("Median per-run RTT p99: N/A")

        print()

        if payload_values:
            unique_payloads = sorted(set(payload_values))

            if len(unique_payloads) == 1:
                payload = unique_payloads[0]
                print(
                    f"Payload size: {payload} bytes "
                    f"({payload / (1024 * 1024):.2f} MiB)"
                )
            else:
                print("Payload sizes:")
                for payload in unique_payloads:
                    print(
                        f"  {payload} bytes "
                        f"({payload / (1024 * 1024):.2f} MiB)"
                    )

            print()

        if failed_runs or incomplete_success_runs:
            print("--- Problematic Runs ---")

            if failed_runs:
                print(
                    "Failed: "
                    + ", ".join(
                        str(row["_run"]) for row in failed_runs
                    )
                )

            if incomplete_success_runs:
                print(
                    "Incomplete: "
                    + ", ".join(
                        str(run) for run in incomplete_success_runs
                    )
                )

            print()

        if args.summary_output:
            summary_path = Path(args.summary_output)
            summary_path.parent.mkdir(
                parents=True,
                exist_ok=True,
            )

            summary_rows = [
                ("total_runs", total_runs),
                ("successful_runs", len(successful_runs)),
                ("failed_runs", len(failed_runs)),
                (
                    "incomplete_success_runs",
                    len(incomplete_success_runs),
                ),
                ("success_rate_percent", success_rate),
                (
                    "transfer_time_mean_ms",
                    mean_or_nan(transfer_times),
                ),
                (
                    "transfer_time_median_ms",
                    median_or_nan(transfer_times),
                ),
                (
                    "transfer_time_p50_ms",
                    percentile(transfer_times, 50),
                ),
                (
                    "transfer_time_p95_ms",
                    percentile(transfer_times, 95),
                ),
                (
                    "transfer_time_p99_ms",
                    percentile(transfer_times, 99),
                ),
                (
                    "goodput_mean_mbps",
                    mean_or_nan(goodputs),
                ),
                (
                    "goodput_median_mbps",
                    median_or_nan(goodputs),
                ),
                (
                    "goodput_p50_mbps",
                    percentile(goodputs, 50),
                ),
                (
                    "goodput_p95_mbps",
                    percentile(goodputs, 95),
                ),
                (
                    "goodput_p99_mbps",
                    percentile(goodputs, 99),
                ),
                (
                    "total_initial_data_sends",
                    total_initial,
                ),
                (
                    "total_retransmissions",
                    total_retransmissions,
                ),
                (
                    "overall_retransmission_rate",
                    overall_retransmission_rate,
                ),
                (
                    "total_acks_received",
                    total_acks,
                ),
                (
                    "total_rtt_samples",
                    total_rtt_samples,
                ),
            ]

            with summary_path.open(
                "w",
                newline="",
                encoding="utf-8",
            ) as handle:
                writer = csv.writer(handle)
                writer.writerow(["metric", "value"])
                writer.writerows(summary_rows)

            print(f"Summary CSV: {summary_path}")

        return 0

    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
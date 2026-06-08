#!/usr/bin/env python3
"""
monolith.py — A monolithic data pipeline that desperately needs refactoring.
Does: config loading, CSV parsing, data validation, API calls, report generation,
email notification, and file cleanup — all in one 300-line blob with no structure.
"""
import os, sys, csv, json, smtplib, requests, logging, shutil, hashlib
from datetime import datetime, timedelta
from email.mime.text import MIMEText
from pathlib import Path

# ─── Global config hacked together ───
CONFIG = {}
def load_config(path="config.json"):
    global CONFIG
    try:
        with open(path) as f:
            CONFIG = json.load(f)
    except:
        CONFIG = {
            "api_base": "https://httpbin.org",
            "data_dir": "./data",
            "output_dir": "./output",
            "archive_dir": "./archive",
            "email": {"smtp_host": "localhost", "smtp_port": 25,
                       "from": "pipeline@localhost", "to": "admin@localhost"},
            "batch_size": 100,
            "max_retries": 3
        }
    os.makedirs(CONFIG.get("data_dir", "./data"), exist_ok=True)
    os.makedirs(CONFIG.get("output_dir", "./output"), exist_ok=True)
    os.makedirs(CONFIG.get("archive_dir", "./archive"), exist_ok=True)

# ─── Logging set up inline ───
LOG_FILE = None
def setup_logging():
    global LOG_FILE
    log_dir = CONFIG.get("output_dir", ".") + "/logs"
    os.makedirs(log_dir, exist_ok=True)
    LOG_FILE = log_dir + "/pipeline.log"
    logging.basicConfig(filename=LOG_FILE, level=logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s")

# ─── CSV parsing with validation inline ───
def parse_csv(filepath):
    rows = []
    errors = []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for i, row in enumerate(reader):
            # Validate required fields
            if not row.get('id'):
                errors.append(f"Row {i}: missing id")
                continue
            if not row.get('value'):
                errors.append(f"Row {i}: missing value")
                continue
            # Parse numeric fields
            try:
                row['value'] = float(row['value'])
            except:
                errors.append(f"Row {i}: invalid value '{row.get('value')}'")
                continue
            # Parse date if present
            if row.get('date'):
                try:
                    datetime.strptime(row['date'], '%Y-%m-%d')
                except:
                    errors.append(f"Row {i}: invalid date '{row['date']}'")
                    row['date'] = None
            # Hash the row for dedup
            row['_hash'] = hashlib.md5(json.dumps(row, sort_keys=True, default=str).encode()).hexdigest()
            rows.append(row)
    return rows, errors

# ─── Deduplication logic mixed in ───
def deduplicate(rows):
    seen = set()
    unique = []
    dups = 0
    for row in rows:
        h = row.get('_hash', '')
        if h in seen:
            dups += 1
            continue
        seen.add(h)
        unique.append(row)
    return unique, dups

# ─── API enrichment (batch calls) ───
def enrich_via_api(rows):
    enriched = []
    failures = 0
    api_base = CONFIG.get("api_base", "https://httpbin.org")
    batch_size = CONFIG.get("batch_size", 100)
    max_retries = CONFIG.get("max_retries", 3)

    for i in range(0, len(rows), batch_size):
        batch = rows[i:i+batch_size]
        payload = {"items": [{"id": r['id'], "value": r['value']} for r in batch]}
        for attempt in range(max_retries):
            try:
                resp = requests.post(f"{api_base}/post", json=payload, timeout=10)
                if resp.status_code == 200:
                    data = resp.json()
                    # Extract enrichment from response
                    for j, item in enumerate(batch):
                        enriched_data = data.get('json', {}).get('items', [])
                        if j < len(enriched_data):
                            item['enriched'] = enriched_data[j]
                        else:
                            item['enriched'] = {}
                        item['_api_status'] = 'ok'
                        enriched.append(item)
                    break
                else:
                    if attempt == max_retries - 1:
                        for item in batch:
                            item['_api_status'] = f'error_{resp.status_code}'
                            enriched.append(item)
                        failures += len(batch)
            except Exception as e:
                if attempt == max_retries - 1:
                    for item in batch:
                        item['_api_status'] = f'error_{str(e)[:50]}'
                        enriched.append(item)
                    failures += len(batch)
    return enriched, failures

# ─── Report generation ───
def generate_report(rows, stats):
    report_path = CONFIG.get("output_dir", ".") + "/report_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".md"
    with open(report_path, 'w') as f:
        f.write(f"# Data Pipeline Report\n\n")
        f.write(f"Generated: {datetime.now().isoformat()}\n\n")
        f.write(f"## Statistics\n\n")
        f.write(f"- Total rows processed: {stats['total']}\n")
        f.write(f"- Valid rows: {stats['valid']}\n")
        f.write(f"- Duplicates removed: {stats['duplicates']}\n")
        f.write(f"- API enrich failures: {stats['api_failures']}\n")
        f.write(f"- Validation errors: {stats['validation_errors']}\n\n")
        f.write(f"## Sample Data\n\n")
        f.write(f"| ID | Value | Status |\n")
        f.write(f"|----|-------|--------|\n")
        for row in rows[:20]:
            f.write(f"| {row.get('id','?')} | {row.get('value','?')} | {row.get('_api_status','?')} |\n")
        f.write(f"\n*{len(rows)} total rows in output*\n")
    return report_path

# ─── Email notification ───
def send_email(report_path, stats):
    email_cfg = CONFIG.get("email", {})
    if not email_cfg.get("smtp_host"):
        logging.warning("No SMTP config, skipping email")
        return False

    body = f"Pipeline completed.\n"
    body += f"Total: {stats['total']}, Valid: {stats['valid']}, Errors: {stats['validation_errors']}\n"
    body += f"Report: {report_path}\n"

    msg = MIMEText(body)
    msg['Subject'] = f"Pipeline Report - {datetime.now().strftime('%Y-%m-%d')}"
    msg['From'] = email_cfg.get('from', 'pipeline@localhost')
    msg['To'] = email_cfg.get('to', 'admin@localhost')

    try:
        s = smtplib.SMTP(email_cfg['smtp_host'], email_cfg.get('smtp_port', 25))
        s.send_message(msg)
        s.quit()
        return True
    except Exception as e:
        logging.error(f"Email failed: {e}")
        return False

# ─── File archival ───
def archive_files(source_path, archive_dir):
    archive_dir = archive_dir or CONFIG.get("archive_dir", "./archive")
    archive_path = os.path.join(archive_dir, f"input_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv")
    shutil.copy2(source_path, archive_path)
    # Create metadata file
    meta = {
        "source": source_path,
        "archived_at": datetime.now().isoformat(),
        "size": os.path.getsize(source_path)
    }
    with open(archive_path + ".meta.json", 'w') as f:
        json.dump(meta, f)
    return archive_path

# ─── Main pipeline ───
def main():
    if len(sys.argv) < 2:
        print("Usage: python monolith.py <input.csv> [config.json]")
        sys.exit(1)

    input_file = sys.argv[1]
    config_file = sys.argv[2] if len(sys.argv) > 2 else "config.json"

    print(f"=== Data Pipeline ===")
    print(f"Input: {input_file}")
    print(f"Config: {config_file}")

    # Step 1: Load config
    load_config(config_file)
    setup_logging()
    logging.info("Pipeline started")

    # Step 2: Parse CSV
    print("Parsing CSV...")
    rows, errors = parse_csv(input_file)
    logging.info(f"Parsed {len(rows)} rows, {len(errors)} errors")

    # Step 3: Deduplicate
    print("Deduplicating...")
    unique_rows, dup_count = deduplicate(rows)
    logging.info(f"Deduplicated: {dup_count} duplicates removed, {len(unique_rows)} unique")

    # Step 4: Enrich via API
    print(f"Enriching {len(unique_rows)} rows via API...")
    enriched_rows, api_failures = enrich_via_api(unique_rows)
    logging.info(f"Enriched: {api_failures} API failures")

    # Step 5: Generate report
    print("Generating report...")
    stats = {
        "total": len(rows),
        "valid": len(unique_rows),
        "duplicates": dup_count,
        "api_failures": api_failures,
        "validation_errors": len(errors)
    }
    report_path = generate_report(enriched_rows, stats)
    logging.info(f"Report generated: {report_path}")

    # Step 6: Send email
    print("Sending notification...")
    sent = send_email(report_path, stats)
    if sent:
        logging.info("Email sent")
    else:
        logging.info("Email skipped (no SMTP config)")

    # Step 7: Archive
    print("Archiving input...")
    archive_path = archive_files(input_file, None)
    logging.info(f"Archived to {archive_path}")

    # Step 8: Save output
    output_path = CONFIG.get("output_dir", ".") + "/enriched_output.json"
    with open(output_path, 'w') as f:
        json.dump({"rows": enriched_rows, "stats": stats, "errors": errors}, f, indent=2, default=str)
    logging.info(f"Output saved to {output_path}")

    # Step 9: Cleanup old files
    print("Cleaning up old files...")
    archive_dir = CONFIG.get("archive_dir", "./archive")
    cutoff = datetime.now() - timedelta(days=30)
    cleaned = 0
    for f in os.listdir(archive_dir):
        fpath = os.path.join(archive_dir, f)
        if os.path.isfile(fpath):
            mtime = datetime.fromtimestamp(os.path.getmtime(fpath))
            if mtime < cutoff:
                os.remove(fpath)
                cleaned += 1
    logging.info(f"Cleaned {cleaned} old archive files")

    print(f"\n=== Pipeline Complete ===")
    print(f"Rows: {stats['total']} → {stats['valid']} valid, {dup_count} dups removed")
    print(f"API failures: {api_failures}")
    print(f"Report: {report_path}")
    print(f"Output: {output_path}")

if __name__ == "__main__":
    main()

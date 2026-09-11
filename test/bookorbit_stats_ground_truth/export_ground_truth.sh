#!/usr/bin/env bash
# Exports events and KOReader's own computed statistics from a real
# statistics.sqlite3, so the C++ implementation can be checked against numbers
# SQLite produced rather than against our reading of the SQL.
#
# Usage: ./export_ground_truth.sh <path-to-statistics.sqlite3> <out-dir>
set -euo pipefail

DB="${1:?path to statistics.sqlite3 required}"
OUT="${2:?output directory required}"
MAX_SEC=120

mkdir -p "$OUT"

# Raw events, ordered as the uploader orders them.
sqlite3 -noheader -csv "$DB" "
  SELECT id_book, page, start_time, duration, total_pages
  FROM page_stat_data
  WHERE total_pages > 0
  ORDER BY id_book, start_time, page;
" > "$OUT/events.csv"

# KOReader's own answers, straight from its SQL.
# Capped totals group by page, per STATISTICS_SQL_BOOK_CAPPED_TOTALS_QUERY.
sqlite3 -noheader -csv "$DB" "
  SELECT b.id,
         (SELECT count(DISTINCT page) FROM page_stat_data WHERE id_book = b.id AND total_pages > 0),
         (SELECT sum(duration)        FROM page_stat_data WHERE id_book = b.id AND total_pages > 0),
         (SELECT sum(capped) FROM (
             SELECT min(sum(duration), $MAX_SEC) AS capped
             FROM page_stat_data WHERE id_book = b.id AND total_pages > 0
             GROUP BY page)),
         (SELECT count(DISTINCT date(start_time, 'unixepoch'))
            FROM page_stat_data WHERE id_book = b.id AND total_pages > 0)
  FROM book b
  ORDER BY b.id;
" > "$OUT/expected.csv"

echo "wrote $OUT/events.csv ($(wc -l < "$OUT/events.csv") rows)"
echo "wrote $OUT/expected.csv ($(wc -l < "$OUT/expected.csv") rows)"

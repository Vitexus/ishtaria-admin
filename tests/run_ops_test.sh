#!/bin/sh
# usage: run_ops_test.sh <ops_test binary> <ishtaria-server binary> <pgm> <server.toml>
# Builds a throw-away database with the server's migrations, runs the test, drops it.
set -e
TEST="$1"; SERVER="$2"; PGM="$3"; CONFIG="$4"
[ -x "$SERVER" ] || { echo "ishtaria-server not found, skipping"; exit 77; }
DB="ishtaria_admin_test_$$"
HOST="${PGHOST:-/var/run/postgresql}"
createdb -h "$HOST" "$DB" || exit 77
trap 'dropdb -h "$HOST" --if-exists "$DB"' EXIT
URL="postgresql:///$DB?host=$HOST"
DATABASE_URL="$URL" "$SERVER" "$CONFIG" --import "$PGM" --seed 42 --import-only
# Like the server binary above, prefer the freshly built worldgen of the sibling checkout
# (the 16-bit PGM strip the admin validates); fall back to the installed one by hand.
WORLDGEN="${ISHTARIA_WORLDGEN:-}"
if [ -z "$WORLDGEN" ]; then
    SIBLING="$(dirname "$0")/../../ishtaria-worldgen/target/debug/ishtaria-worldgen"
    if [ -x "$SIBLING" ]; then
        WORLDGEN="$SIBLING"
    else
        WORLDGEN="ishtaria-worldgen"
    fi
fi
ISHTARIA_SERVER="$SERVER" ISHTARIA_SERVER_CONFIG="$CONFIG" ISHTARIA_DATADISK_DIR="$(dirname "$0")/datadisks" ISHTARIA_WORLDGEN="$WORLDGEN" DATABASE_URL="$URL" "$TEST"

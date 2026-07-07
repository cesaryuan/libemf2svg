#!/bin/sh
#
# Snapshot helper for the EMF-to-SVG converter.
#
# This script has two jobs:
#   1. `save` converts the current EMF fixtures into SVG files and stores them
#      as a snapshot baseline.
#   2. `check` converts the same fixtures into a temporary output directory and
#      compares the result with the saved baseline.
#
# The snapshot is intentionally text-based SVG output instead of hashes, because
# rendering regressions in this project are often easiest to inspect with a
# normal diff or by opening the changed SVG files.

if [ "`uname`" = "Darwin" ]
then
    RL=greadlink
else
    RL=readlink
fi

ABSPATH=$($RL -f "$(dirname "$0")")
CALLER_PWD=`pwd -P`
TEST_ROOT=$($RL -f "$ABSPATH/..")
ACTION="$1"
EMFDIR="$ABSPATH/emf"
SNAPSHOT_DIR="$TEST_ROOT/snapshots/emf"
WORKDIR="$TEST_ROOT/snapshot-out/emf"
RESIZE_OPTS=""
VERBOSE_OPT=""
ret=0

if [ "$ACTION" = "-h" ] || [ "$ACTION" = "--help" ]
then
    ACTION="help"
fi

if [ -n "$ACTION" ]
then
    shift
fi

# Resolve a user-provided path relative to the caller without requiring it
# to already exist. This is needed because `save` creates the snapshot dir.
absolute_path(){
    path="$1"
    if [ "${path#/}" = "$path" ]
    then
        printf "%s/%s\n" "$CALLER_PWD" "$path"
    else
        printf "%s\n" "$path"
    fi
}

# Print command help for creating or checking SVG snapshots.
help(){
    cat <<EOF
usage: `basename "$0"` save|check [-h] [-e <emf dir>] [-d <snapshot dir>] [-o <work dir>] [-r] [-v]

Create or compare SVG snapshots for emf2svg-conv.

actions:
  save   convert EMF files and overwrite the snapshot baseline
  check  convert EMF files and compare them with the snapshot baseline

arguments:
  -h: display this help
  -e: alternate emf dir (default '$EMFDIR')
  -d: snapshot dir (default '$SNAPSHOT_DIR')
  -o: temporary output dir for check (default '$WORKDIR')
  -r: resize to 800x600 before snapshotting
  -v: verbose converter output
EOF
}

# Print a consistent status line so branch comparisons are easy to scan.
log_info(){
    printf "[snapshot] %s\n" "$1"
}

# Convert every EMF file in the input directory into a matching SVG snapshot.
generate_snapshots(){
    src_dir="$1"
    dst_dir="$2"
    cmd="$3"

    rm -rf "$dst_dir"
    mkdir -p "$dst_dir"

    for emf in `find "$src_dir" -type f -name "*.emf" | sort`
    do
        rel="${emf#$src_dir/}"
        svg="$dst_dir/$rel.svg"
        mkdir -p "`dirname "$svg"`"
        log_info "convert $rel"
        "$cmd" -p $RESIZE_OPTS -i "$emf" -o "$svg" $VERBOSE_OPT
        tmpret=$?
        if [ $tmpret -ne 0 ]
        then
            printf "[snapshot] ERROR: emf2svg-conv failed on '%s'\n" "$emf"
            ret=1
        fi
    done
}

while getopts ":he:d:o:rv" opt; do
  case $opt in
    h)
        help
        exit 0
        ;;
    e)
        EMFDIR="$OPTARG"
        ;;
    d)
        SNAPSHOT_DIR="$OPTARG"
        ;;
    o)
        WORKDIR="$OPTARG"
        ;;
    r)
        RESIZE_OPTS="-w 800 -h 600"
        ;;
    v)
        VERBOSE_OPT="--verbose"
        ;;
    \?)
        echo "Invalid option: -$OPTARG" >&2
        help
        exit 1
        ;;
    :)
        echo "Option -$OPTARG requires an argument." >&2
        help
        exit 1
        ;;
  esac
done

if [ "$ACTION" = "help" ]
then
    help
    exit 0
fi

if ! [ "$ACTION" = "save" ] && ! [ "$ACTION" = "check" ]
then
    help
    exit 1
fi

cd "$ABSPATH" || exit 1
. ./colors.sh

CMD="`$RL -f ../../build/emf2svg-conv`"
EMFDIR="`absolute_path "$EMFDIR"`"
SNAPSHOT_DIR="`absolute_path "$SNAPSHOT_DIR"`"
WORKDIR="`absolute_path "$WORKDIR"`"

if ! [ -x "$CMD" ]
then
    printf "[%bFAIL%b] missing converter: %s\n" "$BRed" "$RCol" "$CMD"
    printf "Build it first, for example: cmake --build build --target emf2svg-conv\n"
    exit 1
fi

if ! [ -d "$EMFDIR" ]
then
    printf "[%bFAIL%b] missing EMF directory: %s\n" "$BRed" "$RCol" "$EMFDIR"
    exit 1
fi
EMFDIR="`cd "$EMFDIR" && pwd -P`"

if [ "$ACTION" = "save" ]
then
    log_info "saving snapshots to $SNAPSHOT_DIR"
    generate_snapshots "$EMFDIR" "$SNAPSHOT_DIR" "$CMD"
    if [ $ret -ne 0 ]
    then
        printf "[%bFAIL%b] Snapshot save failed\n" "$BRed" "$RCol"
        exit $ret
    fi
    printf "[%bSUCCESS%b] Snapshot saved: %s\n" "$BGre" "$RCol" "$SNAPSHOT_DIR"
    exit 0
fi

if ! [ -d "$SNAPSHOT_DIR" ]
then
    printf "[%bFAIL%b] missing snapshot baseline: %s\n" "$BRed" "$RCol" "$SNAPSHOT_DIR"
    printf "Create it first with: ./tests/resources/snapshot.sh save\n"
    exit 1
fi

log_info "checking snapshots against $SNAPSHOT_DIR"
generate_snapshots "$EMFDIR" "$WORKDIR" "$CMD"
if [ $ret -ne 0 ]
then
    printf "[%bFAIL%b] Snapshot generation failed\n" "$BRed" "$RCol"
    exit $ret
fi

diff -ru "$SNAPSHOT_DIR" "$WORKDIR"
ret=$?
if [ $ret -ne 0 ]
then
    printf "[%bFAIL%b] Snapshot changed\n" "$BRed" "$RCol"
    printf "Expected: %s\n" "$SNAPSHOT_DIR"
    printf "Actual  : %s\n" "$WORKDIR"
else
    printf "[%bSUCCESS%b] Snapshot matches\n" "$BGre" "$RCol"
fi

exit $ret

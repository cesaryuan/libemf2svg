#!/bin/sh
#
# Snapshot helper for the EMF and WMF to SVG converters.
#
# This script has two jobs:
#   1. `save` converts the current fixtures into SVG files and stores them
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
FORMAT="emf"
RUN_FORMATS="emf wmf"
EMFDIR=""
EMFDIR_ARG=""
SNAPSHOT_DIR=""
SNAPSHOT_DIR_ARG=""
WORKDIR=""
WORKDIR_ARG=""
INTERMEDIATE_DIR=""
INPUT_EXT=""
FORMAT_LABEL=""
RESIZE_OPTS=""
VERBOSE_OPT=""
DETAIL_DIFF=""
CHANGED_LIST=""
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

# Configure format-specific inputs, outputs, and converter requirements.
configure_format(){
    INTERMEDIATE_DIR=""
    case "$FORMAT" in
        emf)
            INPUT_EXT="emf"
            FORMAT_LABEL="EMF"
            DEFAULT_EMFDIR="$ABSPATH/emf"
            DEFAULT_SNAPSHOT_DIR="$TEST_ROOT/snapshots/emf"
            DEFAULT_WORKDIR="$TEST_ROOT/snapshot-out/emf"
            ;;
        wmf)
            INPUT_EXT="wmf"
            FORMAT_LABEL="WMF"
            DEFAULT_EMFDIR="$ABSPATH/wmf"
            DEFAULT_SNAPSHOT_DIR="$TEST_ROOT/snapshots/wmf"
            DEFAULT_WORKDIR="$TEST_ROOT/snapshot-out/wmf"
            INTERMEDIATE_DIR="$TEST_ROOT/snapshot-out/wmf-emf"
            ;;
        *)
            printf "[snapshot] invalid format '%s'; expected 'emf' or 'wmf'\n" "$FORMAT" >&2
            return 1
            ;;
    esac

    if [ -n "$EMFDIR_ARG" ]
    then
        EMFDIR="$EMFDIR_ARG"
    else
        EMFDIR="$DEFAULT_EMFDIR"
    fi
    if [ -n "$SNAPSHOT_DIR_ARG" ]
    then
        SNAPSHOT_DIR="$SNAPSHOT_DIR_ARG"
    else
        SNAPSHOT_DIR="$DEFAULT_SNAPSHOT_DIR"
    fi
    if [ -n "$WORKDIR_ARG" ]
    then
        WORKDIR="$WORKDIR_ARG"
    else
        WORKDIR="$DEFAULT_WORKDIR"
    fi
}

# Print command help for creating or checking SVG snapshots.
help(){
    configure_format || exit 1
    cat <<EOF
usage: `basename "$0"` save|check [-h] [-e <input dir>] [-d <snapshot dir>] [-o <work dir>] [-r] [-v] [--format emf|wmf] [--detail-diff <sample path>]

Create or compare SVG snapshots for emf2svg-conv and wmf2emf-conv.
Without --format, save and check run both EMF and WMF snapshots.

actions:
  save   convert input files and overwrite the snapshot baseline
  check  convert input files and compare them with the snapshot baseline

arguments:
  -h: display this help
  -e: alternate input dir (use with --format for one format)
  -d: snapshot dir (use with --format for one format)
  -o: temporary output dir for check (use with --format for one format)
  --format: snapshot format, 'emf' or 'wmf' (default: both)
  -r: resize to 800x600 before snapshotting
  -v: verbose converter output
  --detail-diff: print a unified diff for one changed sample
EOF
}

# Print a consistent status line so branch comparisons are easy to scan.
log_info(){
    printf "[snapshot] %s\n" "$1"
}

# Print a file only when verbose mode is enabled, keeping failed checks focused
# on the samples that actually changed.
log_convert(){
    if [ -n "$VERBOSE_OPT" ]
    then
        log_info "convert $1"
    fi
}

# Convert one input file into a matching SVG snapshot.
convert_snapshot(){
    input="$1"
    output="$2"
    rel="$3"

    if [ "$FORMAT" = "wmf" ]
    then
        emf_tmp="$INTERMEDIATE_DIR/$rel.emf"
        mkdir -p "`dirname "$emf_tmp"`"
        "$WMF2EMF_CMD" -i "$input" -o "$emf_tmp" $VERBOSE_OPT
        tmpret=$?
        if [ $tmpret -ne 0 ]
        then
            printf "[snapshot] ERROR: wmf2emf-conv failed on '%s'\n" "$input"
            return $tmpret
        fi
        "$EMF2SVG_CMD" -p $RESIZE_OPTS -i "$emf_tmp" -o "$output" $VERBOSE_OPT
        return $?
    fi

    "$EMF2SVG_CMD" -p $RESIZE_OPTS -i "$input" -o "$output" $VERBOSE_OPT
}

# Convert every input file into a matching SVG snapshot.
generate_snapshots(){
    src_dir="$1"
    dst_dir="$2"

    rm -rf "$dst_dir"
    mkdir -p "$dst_dir"
    if [ "$FORMAT" = "wmf" ]
    then
        rm -rf "$INTERMEDIATE_DIR"
        mkdir -p "$INTERMEDIATE_DIR"
    fi

    for emf in `find "$src_dir" -type f -name "*.$INPUT_EXT" | sort`
    do
        rel="${emf#$src_dir/}"
        svg="$dst_dir/$rel.svg"
        mkdir -p "`dirname "$svg"`"
        log_convert "$rel"
        convert_snapshot "$emf" "$svg" "$rel"
        tmpret=$?
        if [ $tmpret -ne 0 ]
        then
            printf "[snapshot] ERROR: %s snapshot failed on '%s'\n" "$FORMAT_LABEL" "$emf"
            ret=1
        fi
    done
}

# Convert a user-provided sample path to a snapshot-relative SVG path.
snapshot_relpath(){
    sample="$1"

    case "$sample" in
        "$SNAPSHOT_DIR"/*)
            sample="${sample#$SNAPSHOT_DIR/}"
            ;;
        "$WORKDIR"/*)
            sample="${sample#$WORKDIR/}"
            ;;
        "$EMFDIR"/*)
            sample="${sample#$EMFDIR/}.svg"
            ;;
        *.$INPUT_EXT)
            sample="$sample.svg"
            ;;
    esac

    if [ "${sample#/}" = "$sample" ]
    then
        printf "%s\n" "$sample"
    else
        printf "%s\n" "`basename "$sample"`"
    fi
}

# Record changed snapshot files so the default failure output stays compact.
record_changed(){
    printf "%s\n" "$1" >> "$CHANGED_LIST"
}

# Compare generated snapshots with the baseline and print only changed files.
compare_snapshots(){
    expected_dir="$1"
    actual_dir="$2"
    detail_rel="$3"
    ret=0
    total_count=0
    changed_count=0

    : > "$CHANGED_LIST"

    for expected in `find "$expected_dir" -type f -name "*.svg" | sort`
    do
        total_count=$((total_count + 1))
        rel="${expected#$expected_dir/}"
        actual="$actual_dir/$rel"
        if ! [ -f "$actual" ]
        then
            record_changed "$rel"
            ret=1
            continue
        fi
        if ! cmp -s "$expected" "$actual"
        then
            record_changed "$rel"
            ret=1
        fi
    done

    for actual in `find "$actual_dir" -type f -name "*.svg" | sort`
    do
        rel="${actual#$actual_dir/}"
        expected="$expected_dir/$rel"
        if ! [ -f "$expected" ]
        then
            total_count=$((total_count + 1))
            record_changed "$rel"
            ret=1
        fi
    done

    sort -u "$CHANGED_LIST" > "$CHANGED_LIST.sorted"
    changed_count=`wc -l < "$CHANGED_LIST.sorted" | tr -d ' '`

    if [ $ret -ne 0 ]
    then
        cat "$CHANGED_LIST.sorted"
        log_info "changed: $changed_count / $total_count"
        if [ -n "$detail_rel" ]
        then
            printf "\n"
            if grep -qx "$detail_rel" "$CHANGED_LIST.sorted"
            then
                diff -u "$expected_dir/$detail_rel" "$actual_dir/$detail_rel"
            else
                printf "[snapshot] '%s' is not in the changed snapshot list\n" "$detail_rel"
            fi
        fi
    else
        log_info "changed: 0 / $total_count"
    fi

    return $ret
}

# Validate converter paths needed by the selected formats.
validate_common_converters(){
    if ! [ -x "$EMF2SVG_CMD" ]
    then
        printf "[%bFAIL%b] missing converter: %s\n" "$BRed" "$RCol" "$EMF2SVG_CMD"
        printf "Build it first, for example: cmake --build build --target emf2svg-conv\n"
        return 1
    fi
    case " $RUN_FORMATS " in
        *" wmf "*)
            ;;
        *)
            return 0
            ;;
    esac

    if ! [ -x "$WMF2EMF_CMD" ]
    then
        printf "[%bFAIL%b] missing converter: %s\n" "$BRed" "$RCol" "$WMF2EMF_CMD"
        printf "Build it first, for example: cmake --build build --target wmf2emf-conv\n"
        return 1
    fi
}

# Keep one-format directory overrides from being applied to both snapshot trees.
validate_directory_overrides(){
    case "$RUN_FORMATS" in
        *" "*)
            if [ -n "$EMFDIR_ARG" ] || [ -n "$SNAPSHOT_DIR_ARG" ] || [ -n "$WORKDIR_ARG" ]
            then
                printf "[snapshot] -e, -d, and -o require --format emf or --format wmf\n" >&2
                return 1
            fi
            ;;
    esac
}

# Run save or check for one concrete format; the default CLI calls this twice.
run_snapshot_action(){
    FORMAT="$1"
    ret=0

    configure_format || return 1
    EMFDIR="`absolute_path "$EMFDIR"`"
    SNAPSHOT_DIR="`absolute_path "$SNAPSHOT_DIR"`"
    WORKDIR="`absolute_path "$WORKDIR"`"

    if ! [ -d "$EMFDIR" ]
    then
        printf "[%bFAIL%b] missing %s directory: %s\n" "$BRed" "$RCol" "$FORMAT_LABEL" "$EMFDIR"
        return 1
    fi
    EMFDIR="`cd "$EMFDIR" && pwd -P`"

    log_info "format: $FORMAT"

    if [ "$ACTION" = "save" ]
    then
        log_info "saving snapshots to $SNAPSHOT_DIR"
        generate_snapshots "$EMFDIR" "$SNAPSHOT_DIR"
        if [ $ret -ne 0 ]
        then
            printf "[%bFAIL%b] %s snapshot save failed\n" "$BRed" "$RCol" "$FORMAT_LABEL"
            return $ret
        fi
        printf "[%bSUCCESS%b] %s snapshot saved: %s\n" "$BGre" "$RCol" "$FORMAT_LABEL" "$SNAPSHOT_DIR"
        return 0
    fi

    if ! [ -d "$SNAPSHOT_DIR" ]
    then
        printf "[%bFAIL%b] missing snapshot baseline: %s\n" "$BRed" "$RCol" "$SNAPSHOT_DIR"
        printf "Create it first with: ./tests/resources/snapshot.sh save --format %s\n" "$FORMAT"
        return 1
    fi
    SNAPSHOT_DIR="`cd "$SNAPSHOT_DIR" && pwd -P`"

    log_info "checking snapshots against $SNAPSHOT_DIR"
    generate_snapshots "$EMFDIR" "$WORKDIR"
    if [ $ret -ne 0 ]
    then
        printf "[%bFAIL%b] %s snapshot generation failed\n" "$BRed" "$RCol" "$FORMAT_LABEL"
        return $ret
    fi
    WORKDIR="`cd "$WORKDIR" && pwd -P`"

    CHANGED_LIST="$TEST_ROOT/snapshot-out/$FORMAT-changed-files.txt"
    mkdir -p "`dirname "$CHANGED_LIST"`"
    detail_diff="$DETAIL_DIFF"
    if [ -n "$detail_diff" ]
    then
        detail_diff="`snapshot_relpath "$detail_diff"`"
    fi

    compare_snapshots "$SNAPSHOT_DIR" "$WORKDIR" "$detail_diff"
    ret=$?
    if [ $ret -ne 0 ]
    then
        printf "[%bFAIL%b] %s snapshot changed\n" "$BRed" "$RCol" "$FORMAT_LABEL"
        printf "Expected: %s\n" "$SNAPSHOT_DIR"
        printf "Actual  : %s\n" "$WORKDIR"
    else
        printf "[%bSUCCESS%b] %s snapshot matches\n" "$BGre" "$RCol" "$FORMAT_LABEL"
    fi

    return $ret
}

while [ $# -gt 0 ]
do
  opt="$1"
  case "$opt" in
    -h)
        help
        exit 0
        ;;
    -e)
        shift
        if [ $# -eq 0 ]
        then
            echo "Option -e requires an argument." >&2
            help
            exit 1
        fi
        EMFDIR_ARG="$1"
        ;;
    -d)
        shift
        if [ $# -eq 0 ]
        then
            echo "Option -d requires an argument." >&2
            help
            exit 1
        fi
        SNAPSHOT_DIR_ARG="$1"
        ;;
    -o)
        shift
        if [ $# -eq 0 ]
        then
            echo "Option -o requires an argument." >&2
            help
            exit 1
        fi
        WORKDIR_ARG="$1"
        ;;
    --format)
        shift
        if [ $# -eq 0 ]
        then
            echo "Option --format requires an argument." >&2
            help
            exit 1
        fi
        RUN_FORMATS="$1"
        ;;
    -r)
        RESIZE_OPTS="-w 800 -h 600"
        ;;
    -v)
        VERBOSE_OPT="--verbose"
        ;;
    --detail-diff)
        shift
        if [ $# -eq 0 ]
        then
            echo "Option --detail-diff requires an argument." >&2
            help
            exit 1
        fi
        DETAIL_DIFF="$1"
        ;;
    --)
        shift
        break
        ;;
    -*)
        echo "Invalid option: $opt" >&2
        help
        exit 1
        ;;
    *)
        echo "Invalid argument: $opt" >&2
        help
        exit 1
        ;;
  esac
  shift
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

EMF2SVG_CMD="`$RL -f ../../build/emf2svg-conv`"
WMF2EMF_CMD="`$RL -f ../../build/wmf2emf-conv`"
validate_directory_overrides || exit 1
validate_common_converters || exit 1

final_ret=0
for fmt in $RUN_FORMATS
do
    run_snapshot_action "$fmt"
    tmpret=$?
    if [ $tmpret -ne 0 ]
    then
        final_ret=$tmpret
    fi
done

exit $final_ret

# Licensed under the MIT License
# https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE

#
# Summarize gcov output and fail the build when line coverage is under 100%
#
# Lines annotated with "GCOV_EXCL_LINE", and blocks between "GCOV_EXCL_START" and "GCOV_EXCL_STOP",
# are excluded from the coverage computation. These mark code that cannot be exercised by the unit
# tests - out-of-memory aborts and platform-specific fallbacks.
#

BEGIN {
    FS = ":"
    fileCount = 0
}

FNR == 1 {
    file = FILENAME
    sub(/.*\//, "", file)
    sub(/\.gcov$/, "", file)
    excluding = 0
    if (!(file in covered)) {
        files[fileCount++] = file
        covered[file] = 0
        missed[file] = 0
    }
}

{
    # Split the gcov line into its count, line number, and source text
    count = $1
    gsub(/^[ \t]+|[ \t]+$/, "", count)
    lineNumber = $2 + 0
    ix = index($0, ":")
    rest = substr($0, ix + 1)
    ix = index(rest, ":")
    source = substr(rest, ix + 1)

    # Skip the gcov header lines
    if (lineNumber == 0) {
        next
    }

    # Exclusion markers
    if (source ~ /GCOV_EXCL_START/) {
        excluding = 1
    }
    if (source ~ /GCOV_EXCL_STOP/) {
        excluding = 0
        next
    }
    if (excluding || source ~ /GCOV_EXCL_LINE/) {
        next
    }

    # Tally the line
    if (count == "#####" || count == "=====") {
        missed[file]++
        if (missed[file] <= 10) {
            missLines[file] = missLines[file] " " lineNumber
        }
        if (verbose) {
            printf "%s:%d:%s\n", file, lineNumber, source
        }
    } else if (count != "-") {
        covered[file]++
    }
}

END {
    totalCovered = 0
    totalMissed = 0
    printf "%-24s %8s %8s %9s\n", "File", "Lines", "Missed", "Coverage"
    printf "%-24s %8s %8s %9s\n", "------------------------", "--------", "--------", "---------"
    for (ix = 0; ix < fileCount; ix++) {
        file = files[ix]
        lines = covered[file] + missed[file]
        if (lines == 0) {
            continue
        }
        totalCovered += covered[file]
        totalMissed += missed[file]
        printf "%-24s %8d %8d %8.1f%%\n", file, lines, missed[file], 100 * covered[file] / lines
        if (missed[file] > 0) {
            printf "%-24s   missed lines:%s%s\n", "", missLines[file], (missed[file] > 10 ? " ..." : "")
        }
    }
    totalLines = totalCovered + totalMissed
    printf "%-24s %8s %8s %9s\n", "------------------------", "--------", "--------", "---------"
    if (totalLines == 0) {
        print "ERROR: no coverage data"
        exit 1
    }
    percent = 100 * totalCovered / totalLines
    printf "%-24s %8d %8d %8.1f%%\n", "TOTAL", totalLines, totalMissed, percent
    if (totalMissed != 0) {
        printf "\nFAILED: line coverage is %.1f%%, required 100%%\n", percent
        exit 1
    }
    print "\nPASSED: 100% line coverage"
}

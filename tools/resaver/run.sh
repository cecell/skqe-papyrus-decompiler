#!/usr/bin/env bash
# Compile DumpStacks against ReSaver's shaded jar and run it.
#   tools/resaver/run.sh <save.ess>... [--tsv frames.tsv]
# RESAVER_JAR overrides the jar location (default: the ReSaver repo's target/).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
jar="${RESAVER_JAR:-C:/repos/FallrimTools-Re-Saver/target/ReSaver.jar}"
if [ ! -f "$jar" ]; then jar="$(ls C:/repos/FallrimTools-Re-Saver/target/resaver-*.jar 2>/dev/null | grep -v original | head -1)"; fi
[ -f "$jar" ] || { echo "ReSaver jar not found; build it with: mvn -q package -DskipTests in C:/repos/FallrimTools-Re-Saver" >&2; exit 2; }
out="${TMPDIR:-/tmp}/dumpstacks-classes"
mkdir -p "$out"
if [ ! -f "$out/DumpStacks.class" ] || [ "$here/DumpStacks.java" -nt "$out/DumpStacks.class" ]; then
  javac -nowarn -cp "$jar" -d "$out" "$here/DumpStacks.java"
fi
exec java -Djava.awt.headless=true -Xmx4g -cp "$out;$jar" DumpStacks "$@"

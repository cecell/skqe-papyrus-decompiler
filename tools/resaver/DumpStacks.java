// DumpStacks: a headless driver for ReSaver's save-game parser.
//
// Prints what a Skyrim save records about the Papyrus VM: counts, every active
// script (thread) with its stack frames (script, function, state, program
// counter, the instruction at the counter), and summaries by function and by
// instruction. It is the ground truth behind `pex audit` (docs/AUDIT.md).
//
// Build ReSaver first (C:\repos\FallrimTools-Re-Saver): mvn -q package -DskipTests
// Then:  run.sh <save.ess>... [--tsv frames.tsv]      (see run.sh next to this file)
//
// GPL-3.0-only, like ReSaver and this repository.

import java.io.PrintStream;
import java.lang.reflect.Field;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.*;

import resaver.ProgressModel;
import resaver.ess.ESS;
import resaver.ess.ModelBuilder;
import resaver.ess.papyrus.*;

public class DumpStacks {

    static int ptrOf(StackFrame f) {
        try {
            Field fld = StackFrame.class.getDeclaredField("PTR");
            fld.setAccessible(true);
            return fld.getInt(f);
        } catch (ReflectiveOperationException e) {
            return -1;
        }
    }

    static String opAt(StackFrame f, int ptr) {
        List<OpcodeData> ops = f.getOpcodeData();
        if (ops == null || ptr < 0 || ptr >= ops.size()) return "?";
        return String.valueOf(ops.get(ptr)).replaceAll("\\s+", " ");
    }

    static String callee(String op) {
        // "CALLMETHOD Wait Utility ::temp0 1.0" -> "Utility.Wait"; "CALLSTATIC Utility Wait ..." -> "Utility.Wait"
        String[] t = op.trim().split(" ");
        if (t.length >= 3 && t[0].equalsIgnoreCase("CALLMETHOD")) return t[1];
        if (t.length >= 3 && t[0].equalsIgnoreCase("CALLSTATIC")) return t[1] + "." + t[2];
        if (t.length >= 2 && t[0].equalsIgnoreCase("CALLPARENT")) return "parent." + t[1];
        return t.length > 0 ? t[0] : op;
    }

    static <K> void top(PrintStream out, String title, Map<K, Integer> counts, int n) {
        out.println();
        out.println("== " + title + " (" + counts.values().stream().mapToInt(Integer::intValue).sum() + " total, " + counts.size() + " distinct)");
        counts.entrySet().stream()
            .sorted((a, b) -> b.getValue() - a.getValue())
            .limit(n)
            .forEach(e -> out.printf("%6d  %s%n", e.getValue(), e.getKey()));
    }

    public static void main(String[] args) throws Exception {
        System.setProperty("java.awt.headless", "true");
        List<Path> saves = new ArrayList<>();
        PrintStream tsv = null;
        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--tsv") && i + 1 < args.length) {
                tsv = new PrintStream(args[++i], "UTF-8");
                tsv.println("save\tthread\tdepth\tsuspended\tmessage\tundefined\tscript\tfunction\tstate\tptr\topcode");
            } else saves.add(Paths.get(args[i]));
        }
        PrintStream out = new PrintStream(System.out, true, "UTF-8");

        for (Path save : saves) {
            out.println("##### " + save.getFileName());
            ESS.Result r = ESS.readESS(save, new ModelBuilder(new ProgressModel(1)));
            ESS ess = r.ESS;
            Papyrus p = ess.getPapyrus();
            out.printf("game %s  size %.1f MB  plugins %d%n", r.GAME, r.SIZE_MB,
                ess.getPluginInfo() == null ? 0 : ess.getPluginInfo().getFullPlugins().size() + ess.getPluginInfo().getLitePlugins().size());

            long unattached = p.getScriptInstances().values().stream().filter(ScriptInstance::isUnattached).count();
            long undefInst = p.getScriptInstances().values().stream().filter(ScriptInstance::isUndefined).count();
            long undefScripts = p.getScripts().values().stream().filter(Script::isUndefined).count();
            Collection<ActiveScript> threads = p.getActiveScripts().values();
            long suspended = threads.stream().filter(ActiveScript::isSuspended).count();
            long withMsg = threads.stream().filter(ActiveScript::hasMessage).count();
            long undefThreads = threads.stream().filter(ActiveScript::isUndefined).count();
            long frames = threads.stream().mapToLong(t -> t.getStackFrames().size()).sum();
            int maxDepth = threads.stream().mapToInt(t -> t.getStackFrames().size()).max().orElse(0);

            out.printf("scripts %d (undefined %d)  instances %d (unattached %d, undefined %d)  references %d  arrays %d%n",
                p.getScripts().size(), undefScripts, p.getScriptInstances().size(), unattached, undefInst,
                p.getReferences().size(), p.getArrays().size());
            out.printf("threads %d  suspended %d (table1 %d, table2 %d)  with function message %d  undefined %d  frames %d  deepest %d  function messages %d  queued unbinds %d%n",
                threads.size(), suspended, p.getSuspendedStacks1().size(), p.getSuspendedStacks2().size(), withMsg,
                undefThreads, frames, maxDepth, p.getFunctionMessages().size(), p.getUnbinds().size());

            Map<String, Integer> byTopFn = new HashMap<>(), byAnyFn = new HashMap<>(), byTopOp = new HashMap<>(),
                byScript = new HashMap<>(), byCallee = new HashMap<>(), byMsg = new HashMap<>();

            out.println();
            out.println("== threads (entry frame first, innermost last; '*' suspended, 'M' has function message, 'U' undefined)");
            int n = 0;
            for (ActiveScript t : threads) {
                List<StackFrame> fs = t.getStackFrames();
                String flags = (t.isSuspended() ? "*" : " ") + (t.hasMessage() ? "M" : " ") + (t.isUndefined() ? "U" : " ");
                String msg = "";
                if (t.hasMessage()) {
                    for (FunctionMessage fm : p.getFunctionMessages()) {
                        if (t.getID().equals(fm.getID()) && fm.hasMessage()) {
                            msg = fm.getMessage().getFName().toString();
                            byMsg.merge(msg, 1, Integer::sum);
                        }
                    }
                }
                out.printf("%s %s depth %d%s%n", t.getID(), flags, fs.size(), msg.isEmpty() ? "" : "  message " + msg);
                for (int d = 0; d < fs.size(); d++) {
                    StackFrame f = fs.get(d);
                    int ptr = ptrOf(f);
                    String op = f.isNative() ? "(native)" : opAt(f, ptr);
                    String fn = f.getScriptName() + "." + f.getEvent();
                    String state = f.getStatus() == null ? "" : f.getStatus().toString();
                    out.printf("    %s%s @%d  %s%n", fn, state.isEmpty() ? "" : " [" + state + "]", ptr, op);
                    byAnyFn.merge(fn, 1, Integer::sum);
                    byScript.merge(f.getScriptName().toString(), 1, Integer::sum);
                    if (d == 0) byTopFn.merge(fn, 1, Integer::sum);
                    if (d == fs.size() - 1) {
                        // The innermost frame: for a native it is the native itself (what the thread waits in).
                        byTopOp.merge(f.isNative() ? "native" : op.split(" ")[0], 1, Integer::sum);
                        byCallee.merge(f.isNative() ? fn : callee(op), 1, Integer::sum);
                    }
                    if (tsv != null)
                        tsv.printf("%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%d\t%s%n", save.getFileName(), t.getID(), d,
                            t.isSuspended() ? 1 : 0, t.hasMessage() ? 1 : 0, t.isUndefined() ? 1 : 0,
                            f.getScriptName(), f.getEvent(), state, ptr, op);
                }
                n++;
            }
            top(out, "threads by entry function (the event that started the thread)", byTopFn, 40);
            top(out, "all frames by function", byAnyFn, 40);
            top(out, "frames by script", byScript, 30);
            top(out, "innermost frame: instruction kind", byTopOp, 20);
            top(out, "innermost frame: where the thread is (native) or what it calls", byCallee, 40);
            top(out, "queued function messages (what the game thread was asked to run)", byMsg, 30);

            // The instance population: which scripts have the most instances (timers live per instance).
            Map<String, Integer> byInst = new HashMap<>();
            for (ScriptInstance si : p.getScriptInstances().values()) byInst.merge(String.valueOf(si.getScriptName()), 1, Integer::sum);
            top(out, "script instances by script", byInst, 25);
            out.println();
        }
        if (tsv != null) tsv.close();
    }
}

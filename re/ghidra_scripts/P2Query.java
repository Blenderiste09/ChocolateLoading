// Batch query script for headless Ghidra.
// Usage: analyzeHeadless <projdir> <proj> -process <prog> -noanalysis -readOnly
//        -scriptPath re/ghidra_scripts -postScript P2Query.java <cmdfile> <outfile>
// Command file: one command per line ('#' = comment):
//   decomp <addr|symbol>            decompile function
//   disasm <addr|symbol> [n]        disassemble n instructions (default: whole function)
//   syms <regex>                    symbols whose name matches (case-insensitive)
//   funcs <regex>                   functions whose name matches
//   vtable <addr|symbol> <n>        read n pointers, print target labels
//   xrefs <addr|symbol>             references to address (with containing function)
//   strings <regex>                 defined strings matching + their xrefs
//   bytes <addr|symbol> <n>         hex dump
//   funcat <addr>                   function containing address
//   callers <addr|symbol>           functions calling the function
//   callees <addr|symbol>           functions called by the function
//   findbytes <hexpattern-with-??>  search memory (max 50 hits)
//@category P2
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class P2Query extends GhidraScript {
    private PrintWriter out;
    private DecompInterface decomp;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> cmds = Files.readAllLines(Paths.get(args[0]));
        out = new PrintWriter(new FileWriter(args[1], true));
        out.println("##### program: " + currentProgram.getName() + " base=" + currentProgram.getImageBase());
        for (String line : cmds) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            out.println("\n===== " + line);
            try {
                exec(line);
            } catch (Exception e) {
                out.println("ERROR: " + e);
            }
            out.flush();
        }
        if (decomp != null) decomp.dispose();
        out.close();
    }

    private Address resolve(String s) throws Exception {
        if (s.startsWith("0x") || s.matches("[0-9a-fA-F]{6,}")) {
            return toAddr(s.startsWith("0x") ? s.substring(2) : s);
        }
        SymbolTable st = currentProgram.getSymbolTable();
        SymbolIterator it = st.getSymbols(s);
        while (it.hasNext()) { Symbol sym = it.next(); return sym.getAddress(); }
        // try qualified names like Class::method
        for (Symbol sym : st.getAllSymbols(true)) {
            if (sym.getName(true).equals(s)) return sym.getAddress();
        }
        throw new Exception("unresolved: " + s);
    }

    private String labels(Address a) {
        Symbol[] syms = currentProgram.getSymbolTable().getSymbols(a);
        StringBuilder sb = new StringBuilder();
        for (Symbol s : syms) { if (sb.length() > 0) sb.append(" | "); sb.append(s.getName(true)); }
        Function f = getFunctionContaining(a);
        if (sb.length() == 0 && f != null) sb.append("in ").append(f.getName(true)).append("+0x").append(Long.toHexString(a.subtract(f.getEntryPoint())));
        return sb.toString();
    }

    private void exec(String line) throws Exception {
        String[] p = line.split("\\s+", 3);
        String c = p[0];
        switch (c) {
            case "decomp": {
                Address a = resolve(p[1]);
                Function f = getFunctionContaining(a);
                if (f == null) { createFunction(a, null); f = getFunctionAt(a); }
                if (f == null) { out.println("no function"); return; }
                if (decomp == null) { decomp = new DecompInterface(); decomp.openProgram(currentProgram); }
                DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                out.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("decompile failed: " + r.getErrorMessage()));
                break;
            }
            case "disasm": {
                Address a = resolve(p[1]);
                int n = p.length > 2 ? Integer.parseInt(p[2]) : -1;
                Listing lst = currentProgram.getListing();
                if (n < 0) {
                    Function f = getFunctionContaining(a);
                    if (f == null) { n = 60; }
                    else {
                        out.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
                        InstructionIterator ii = lst.getInstructions(f.getBody(), true);
                        while (ii.hasNext()) printInsn(ii.next());
                        return;
                    }
                }
                Instruction ins = lst.getInstructionAt(a);
                if (ins == null) { disassemble(a); ins = lst.getInstructionAt(a); }
                for (int i = 0; i < n && ins != null; i++) { printInsn(ins); ins = ins.getNext(); }
                break;
            }
            case "syms": {
                Pattern pat = Pattern.compile(p[1], Pattern.CASE_INSENSITIVE);
                int cnt = 0;
                for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
                    String n = s.getName(true);
                    if (pat.matcher(n).find()) {
                        out.println(s.getAddress() + "  " + n + "  [" + s.getSymbolType() + "]");
                        if (++cnt >= 400) { out.println("...truncated"); break; }
                    }
                }
                break;
            }
            case "funcs": {
                Pattern pat = Pattern.compile(p[1], Pattern.CASE_INSENSITIVE);
                int cnt = 0;
                for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                    String n = f.getName(true);
                    if (pat.matcher(n).find()) {
                        out.println(f.getEntryPoint() + "  " + n + "  size=" + f.getBody().getNumAddresses());
                        if (++cnt >= 400) { out.println("...truncated"); break; }
                    }
                }
                break;
            }
            case "vtable": {
                Address a = resolve(p[1]);
                int n = Integer.parseInt(p[2]);
                Memory mem = currentProgram.getMemory();
                for (int i = 0; i < n; i++) {
                    Address slot = a.add(i * 4L);
                    long v = mem.getInt(slot) & 0xffffffffL;
                    Address t = toAddr(v);
                    out.println(String.format("[%3d] +0x%03x  %08x  %s", i, i * 4, v, labels(t)));
                }
                break;
            }
            case "xrefs": {
                Address a = resolve(p[1]);
                int cnt = 0;
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println(r.getFromAddress() + "  " + r.getReferenceType() + "  " + (f != null ? f.getName(true) + " @" + f.getEntryPoint() : ""));
                    if (++cnt >= 200) { out.println("...truncated"); break; }
                }
                break;
            }
            case "strings": {
                Pattern pat = Pattern.compile(p[1]);
                int cnt = 0;
                DataIterator di = currentProgram.getListing().getDefinedData(true);
                while (di.hasNext()) {
                    Data d = di.next();
                    if (!d.hasStringValue()) continue;
                    Object v = d.getValue();
                    if (v == null) continue;
                    String s = v.toString();
                    if (!pat.matcher(s).find()) continue;
                    out.println(d.getAddress() + "  \"" + s.replace("\n", "\\n") + "\"");
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        out.println("      <- " + r.getFromAddress() + "  " + (f != null ? f.getName(true) + " @" + f.getEntryPoint() : ""));
                    }
                    if (++cnt >= 100) { out.println("...truncated"); break; }
                }
                break;
            }
            case "bytes": {
                Address a = resolve(p[1]);
                int n = Integer.parseInt(p[2]);
                byte[] b = new byte[n];
                currentProgram.getMemory().getBytes(a, b);
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < n; i++) {
                    if (i % 16 == 0) sb.append(String.format("%n%s  ", a.add(i)));
                    sb.append(String.format("%02x ", b[i] & 0xff));
                }
                out.println(sb.toString());
                break;
            }
            case "funcat": {
                Address a = resolve(p[1]);
                Function f = getFunctionContaining(a);
                out.println(f == null ? "none" : (f.getName(true) + " @ " + f.getEntryPoint()));
                break;
            }
            case "callers": {
                Address a = resolve(p[1]);
                Function f = getFunctionContaining(a);
                for (Function cf : f.getCallingFunctions(monitor)) out.println(cf.getEntryPoint() + "  " + cf.getName(true));
                break;
            }
            case "callees": {
                Address a = resolve(p[1]);
                Function f = getFunctionContaining(a);
                for (Function cf : f.getCalledFunctions(monitor)) out.println(cf.getEntryPoint() + "  " + cf.getName(true));
                break;
            }
            case "findbytes": {
                String hex = p[1].replace(" ", "");
                if (p.length > 2) hex += p[2].replace(" ", "");
                int len = hex.length() / 2;
                byte[] pb = new byte[len], mb = new byte[len];
                for (int i = 0; i < len; i++) {
                    String h = hex.substring(i * 2, i * 2 + 2);
                    if (h.equals("??")) { pb[i] = 0; mb[i] = 0; }
                    else { pb[i] = (byte) Integer.parseInt(h, 16); mb[i] = (byte) 0xff; }
                }
                Memory mem = currentProgram.getMemory();
                Address start = currentProgram.getMinAddress();
                int cnt = 0;
                while (start != null && cnt < 50) {
                    Address hit = mem.findBytes(start, pb, mb, true, monitor);
                    if (hit == null) break;
                    out.println(hit + "  " + labels(hit));
                    cnt++;
                    start = hit.add(1);
                }
                break;
            }
            default:
                out.println("unknown command");
        }
    }

    private void printInsn(Instruction ins) {
        StringBuilder sb = new StringBuilder();
        sb.append(ins.getAddress()).append("  ").append(String.format("%-40s", ins.toString()));
        for (Reference r : ins.getReferencesFrom()) {
            String l = labels(r.getToAddress());
            if (!l.isEmpty()) sb.append("  ; ").append(l);
        }
        out.println(sb.toString());
    }
}

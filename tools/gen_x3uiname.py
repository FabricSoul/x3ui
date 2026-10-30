#!/usr/bin/env python3
"""Generate addon/scripts/x3uiname.pck — a minimal MSCI script:
     $out = sprintf: fmt='%s', $object, null,null,null,null
     return $out
so the overlay can ask the engine (via run_script_sync) for any object's exact
display name (the same `sprintf '%s' <object>` idiom the game's own UI uses).

codearray encoding (reverse-engineered from x3uimoney + !lib.math.roundnumber):
  top = array[10]: [name, engineVer, desc, scriptVer, 0, varNames[], code[], 0, sourcetext[], 0]
  value pair encoding: null=[0,0]  int=[4,N]  string=[5,"s"]  var-ref=[131074, varIdx]
  sprintf stmt = [149, resultVarIdx, <fmt pair>, <arg1..arg5 pairs>]
  return  stmt = [103, <value pair>]
We reuse x3uimoney's element [8] (sourcetext-in-codearray) verbatim — a known-valid
block — and only author [0]/[5]/[6].
"""
import gzip, html, re, sys, xml.etree.ElementTree as ET

G = "/home/fabric/.local/share/Steam/steamapps/common/X3 Terran Conflict"
money = gzip.decompress(open(G + "/addon/scripts/x3uimoney.pck", "rb").read()).decode("latin1")
ca = re.search(r"<codearray>\s*(<sval.*</sval>)\s*</codearray>", money, re.S).group(1)
top = ET.fromstring(ca)
kids = list(top)
assert len(kids) == 10, f"expected 10 codearray elements, got {len(kids)}"
src8 = ET.tostring(kids[8], encoding="unicode")   # reuse the sourcetext block

def sint(n):  return f'<sval type="int" val="{n}" />'
def sstr(s):  return f'<sval type="string" val="{html.escape(s, quote=True)}" />'
def sarr(it): return f'<sval type="array" size="{len(it)}">' + "".join(it) + "</sval>"

# $out(=var1) = sprintf fmt='%s' $object(=var0), null,null,null,null
sprintf_stmt = sarr([sint(149), sint(1), sint(5), sstr("%s"),
                     sint(131074), sint(0)] + [sint(0)] * 8)   # arg1=$object, args2-5=null
return_stmt  = sarr([sint(103), sint(131074), sint(1)])         # return $out
code  = sarr([sprintf_stmt, return_stmt])
varns = sarr([sstr("object"), sstr("out")])
newtop = sarr([sstr("x3uiname"), sint(50), sstr(""), sint(1), sint(0),
               varns, code, sint(0), src8, sint(0)])

xml = (
    '<?xml version="1.0" standalone="yes"?>\n'
    '<?xml-stylesheet href="x2script.xsl" type="text/xsl"?>\n'
    "<script>\n"
    "<name>x3uiname</name>\n<version>1</version>\n<engineversion>50</engineversion>\n"
    "<description></description>\n"
    '<arguments>\n<argument index="1" name="object" type="Var/Ship" desc="object" />\n</arguments>\n'
    "<sourcetext>\n"
    '<line linenr="001" indent=""><var>$out</var><text> = sprintf: fmt=</text>'
    "<var>'%s'</var><text>, </text><var>$object</var><text>, null, null, null, null</text></line>\n"
    '<line linenr="002" indent=""><text>return </text><var>$out</var></line>\n'
    "</sourcetext>\n"
    f"<codearray>{newtop}</codearray>\n"
    "</script>\n"
)
# sanity: the codearray must re-parse and have 10 top elements
ET.fromstring(re.search(r"<codearray>(<sval.*</sval>)</codearray>", xml, re.S).group(1))
out = G + "/addon/scripts/x3uiname.pck"
open(out, "wb").write(gzip.compress(xml.encode("latin1")))
print("wrote", out)
print("codearray top size:", len(list(ET.fromstring(newtop))))

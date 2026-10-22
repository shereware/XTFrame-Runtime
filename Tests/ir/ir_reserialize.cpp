#include "ir_reserialize.h"

#include <cstring>
#include <string_view>
#include <utility>

#include "xt/compile/ir_writer.h"
#include "xt/ir_pod.h"

// Per-chunk status (T-267c). "rebuilt": every field is read through IrView's public readers and
// written back through IrWriter's public interface. "copied": the chunk's bytes are copied
// verbatim because the public interfaces cannot express it (the reason is given). Rebuilding a
// chunk never uses IrWriter::fromView(), so this test does not grade that function with itself.
//
//   STRT  rebuilt  strings().count() / string(i) -> intern(), in id order
//   ASST  rebuilt  assets().assets() / data()     -> addAsset()
//   STYL  rebuilt  style().*                      -> setStyle()
//   SCEN  rebuilt  scene(i).*                     -> addScene()
//   TLIN  rebuilt  timeline(i).*                  -> addTimeline()
//   DOCT  rebuilt  doc().*                        -> setDoc()
//   SRCM  rebuilt  sourceMap().*                  -> setSourceMap()
//   META  rebuilt  meta().meta()                  -> setMeta()
//   SYMB  rebuilt  symbols().instances()          -> setSymbols()
//   TGRP  rebuilt  timelineGroups().*             -> setTimelineGroups()
//   VARS  rebuilt  vars().*                       -> addVarToken() / addVarFallback()
//   CALC  rebuilt  calcs().calcs() / ops()        -> addCalc() (one call per program)
//   MDIA  rebuilt  media().*                      -> setMedia()
//   STAT  rebuilt  customStates().*               -> setCustomStates()
//   TRIG  rebuilt  triggers().*                   -> setTriggers()
//   STRS  rebuilt  localization().*               -> setLocalization()
//   PORT  rebuilt  ports().*                      -> setPorts()
//   BIND  rebuilt  bindings().*                   -> setBindings()
//   RPT   rebuilt  repeats().repeats()            -> setRepeats()
//   SHDR  rebuilt  shaders().shaders()/params()/text() -> addShader() (param names re-interned)
//
// Copied verbatim: none.

namespace xt::test {

namespace {

template <class T>
std::vector<T> vec(std::span<const T> s) {
    return std::vector<T>(s.begin(), s.end());
}

}  // namespace

std::span<const CopiedChunk> copiedChunks() { return {}; }

std::vector<uint8_t> reserialize(const IrView& view) {
    compile::IrWriter w;

    // STRT
    for (uint32_t i = 0; i < view.strings().count(); ++i) {
        w.intern(view.string(i));
    }

    // ASST
    for (const IrAsset& a : view.assets().assets()) {
        compile::AssetBuilder ab;
        ab.name = a.name;
        ab.kind = a.kind;
        if (a.embedded != 0) {
            std::span<const uint8_t> data = view.assets().data(a);
            ab.embedded.assign(data.begin(), data.end());
        }
        w.addAsset(std::move(ab));
    }

    // STYL
    {
        const StyleChunk& s = view.style();
        compile::StyleBuilder sb;
        sb.rules = vec(s.rules());
        sb.selectorParts = vec(s.selectorParts());
        sb.decls = vec(s.decls());
        sb.keyframesRefs = vec(s.keyframesRefs());
        sb.classIds = vec(s.classIds());
        sb.transitions = vec(s.transitions());
        sb.animations = vec(s.animations());
        sb.fontFamilies = vec(s.fontFamilies());
        sb.fontFaces = vec(s.fontFaces());
        sb.effects = vec(s.effects());
        sb.effectStops = vec(s.effectStops());
        sb.gradients = vec(s.gradients());
        sb.floatPool = vec(s.floatPool());
        sb.easingPoints = vec(s.easingPoints());  // T-335.
        w.setStyle(std::move(sb));
    }

    // SCEN
    for (uint32_t i = 0; i < view.sceneCount(); ++i) {
        const SceneChunk& s = view.scene(i);
        compile::SceneBuilder sb;
        sb.name = s.name();
        sb.width = s.width();
        sb.height = s.height();
        sb.nodes = vec(s.nodes());
        sb.paths = vec(s.paths());
        sb.images = vec(s.images());
        sb.verbPool = vec(s.verbPool());
        sb.floatPool = vec(s.floatPool());
        sb.stopPool = vec(s.stopPool());
        sb.cameras = vec(s.cameras());
        sb.effects = vec(s.effects());
        sb.modifiers = vec(s.modifiers());
        sb.shapes = vec(s.shapes());
        sb.bones = vec(s.bones());
        sb.constraints = vec(s.constraints());
        sb.meshes = vec(s.meshes());
        sb.skins = vec(s.skins());
        sb.tendonPool = vec(s.tendonPool());
        sb.weightPool = vec(s.weightPool());
        sb.meshVec2Pool = vec(s.meshVec2Pool());
        sb.meshIndexPool = vec(s.meshIndexPool());
        w.addScene(std::move(sb));
    }

    // TLIN
    for (uint32_t i = 0; i < view.timelineCount(); ++i) {
        const TimelineChunk& t = view.timeline(i);
        compile::TimelineBuilder tb;
        tb.name = t.name();
        tb.duration = t.duration();
        tb.frameRate = t.frameRate();
        tb.tracks = vec(t.tracks());
        tb.keyframes = vec(t.keyframes());
        tb.floatPool = vec(t.floatPool());
        tb.stopPool = vec(t.stopPool());
        tb.markers = vec(t.markers());
        w.addTimeline(std::move(tb));
    }

    // DOCT
    {
        const DocChunk& d = view.doc();
        compile::DocBuilder db;
        db.elements = vec(d.elements());
        db.texts = vec(d.texts());
        db.attrPool = vec(d.attrPool());
        db.classPool = vec(d.classPool());
        db.inlineDecls = vec(d.inlineDecls());
        db.cameras = vec(d.cameras());
        db.textRuns = vec(d.textRuns());
        db.textInlines = vec(d.textInlines());
        db.textLinks = vec(d.textLinks());
        w.setDoc(std::move(db));
    }

    // SRCM
    if (view.hasSourceMap()) {
        w.setSourceMap(vec(view.sourceMap().fileNames()), vec(view.sourceMap().entries()));
    }

    // META
    if (view.meta().present()) {
        const IrMeta& m = *view.meta().meta();
        w.setMeta(m.sourceFile, m.compilerVersion, m.compileTimeUnix, view.meta().behavior());
    }

    // SYMB
    {
        std::vector<compile::SymbolInstanceBuilder> instances;
        for (const IrSymbolInstance& inst : view.symbols().instances()) {
            compile::SymbolInstanceBuilder ib;
            ib.symbol = inst.symbol;
            ib.instanceElement = inst.instanceElement;
            ib.paths = vec(inst.paths);
            instances.push_back(std::move(ib));
        }
        w.setSymbols(std::move(instances));
    }

    // TGRP
    if (view.timelineGroups().present()) {
        const TimelineGroupChunk& g = view.timelineGroups();
        compile::TimelineGroupBuilder gb;
        gb.groups = vec(g.groups());
        gb.markers = vec(g.markers());
        gb.sounds = vec(g.sounds());
        gb.members = vec(g.members());
        w.setTimelineGroups(std::move(gb));
    }

    // VARS
    for (const IrVar& token : view.vars().tokens()) {
        w.addVarToken(token);
    }
    for (const IrDecl& fallback : view.vars().fallbacks()) {
        w.addVarFallback(fallback);
    }

    // CALC: addCalc() lays the programs' ops out back to back, in program order.
    {
        const CalcChunk& c = view.calcs();
        for (const IrCalc& calc : c.calcs()) {
            std::vector<IrCalcOp> ops;
            if (calc.opOffset <= c.ops().size() && calc.opCount <= c.ops().size() - calc.opOffset) {
                std::span<const IrCalcOp> s = c.ops().subspan(calc.opOffset, calc.opCount);
                ops.assign(s.begin(), s.end());
            }
            w.addCalc(ops);
        }
    }

    // MDIA
    if (view.media().present()) {
        const MediaChunk& m = view.media();
        compile::MediaBuilder mb;
        mb.media = vec(m.media());
        mb.queries = vec(m.queries());
        mb.conds = vec(m.conds());
        mb.rules = vec(m.rules());
        w.setMedia(std::move(mb));
    }

    // STAT
    if (view.customStates().present()) {
        compile::CustomStateBuilder sb;
        sb.names = vec(view.customStates().names());
        sb.groups = vec(view.customStates().groups());
        w.setCustomStates(std::move(sb));
    }

    // TRIG
    if (view.triggers().present()) {
        const TriggerChunk& t = view.triggers();
        compile::TriggerBuilder tb;
        tb.tables = vec(t.tables());
        tb.triggers = vec(t.triggers());
        tb.actions = vec(t.actions());
        tb.conditions = vec(t.conditions());
        w.setTriggers(std::move(tb));
    }

    // STRS
    if (view.localization().present()) {
        const LocalizationChunk& l = view.localization();
        compile::LocalizationBuilder lb;
        lb.keys = vec(l.keys());
        lb.textKeys = vec(l.textKeys());
        lb.attrKeys = vec(l.attrKeys());
        lb.fallbacks = vec(l.fallbacks());
        lb.fallbackFamilies = vec(l.fallbackFamilies());
        w.setLocalization(std::move(lb));
    }

    // PORT
    if (view.ports().present()) {
        compile::PortBuilder pb;
        pb.ports = vec(view.ports().ports());
        pb.fields = vec(view.ports().fields());
        pb.defaults = vec(view.ports().defaults());  // T-358
        pb.nested = vec(view.ports().nested());      // T-391 (T-405: Tests/ir/fixtures/v2_tail.xt)
        w.setPorts(std::move(pb));
    }

    // BIND
    if (view.bindings().present()) {
        const BindingChunk& b = view.bindings();
        compile::BindingBuilder bb;
        bb.bindings = vec(b.bindings());
        bb.sources = vec(b.sources());
        bb.steps = vec(b.steps());
        w.setBindings(std::move(bb));
    }

    // RPT
    if (view.repeats().present()) {
        w.setRepeats(vec(view.repeats().repeats()));
    }

    // SHDR: addShader() re-interns each param name, which finds the existing StringId because
    // STRT was interned first in id order.
    if (view.shaders().present()) {
        const ShaderChunk& sc = view.shaders();
        const std::string_view text = sc.text();
        for (const IrShaderRef& ref : sc.shaders()) {
            compile::ShaderBuilder sb;
            sb.asset = ref.asset;
            if (ref.sourceOffset <= text.size()) {
                sb.source = std::string(text.substr(ref.sourceOffset, ref.sourceLength));
            }
            if (ref.glslOffset <= text.size()) {
                sb.glsl = std::string(text.substr(ref.glslOffset, ref.glslLength));
            }
            for (uint32_t i = 0; i < ref.paramCount && ref.paramOffset + i < sc.params().size(); ++i) {
                const IrShaderParam& p = sc.params()[ref.paramOffset + i];
                sb.params.push_back(p);
                sb.paramNames.emplace_back(view.string(p.name));
            }
            sb.expandPx = ref.expandPx;
            sb.fallback = ref.fallback;
            w.addShader(std::move(sb));
        }
    }

    // CLIP (T-332)
    if (view.clipShapes().present()) {
        const ClipShapeChunk& cc = view.clipShapes();
        for (const IrClipShape& ref : cc.shapes()) {
            ElementClipShape shape;
            shape.kind = static_cast<ClipShapeKind>(ref.kind);
            for (uint32_t i = 0; i < ref.paramCount && ref.paramOffset + i < cc.params().size(); ++i) {
                const IrClipParam& p = cc.params()[ref.paramOffset + i];
                shape.params.push_back(Length{p.value, static_cast<LengthUnit>(p.unit)});
            }
            w.addClipShape(shape);
        }
    }

    // GRID (T-338)
    if (view.grid().present()) {
        const GridChunk& gc = view.grid();
        for (const IrGridList& list : gc.trackLists()) {
            std::vector<GridTrack> tracks;
            for (uint32_t i = 0; i < list.count && list.offset + i < gc.tracks().size(); ++i) {
                const IrGridTrack& t = gc.tracks()[list.offset + i];
                tracks.push_back(GridTrack{static_cast<GridTrackKind>(t.kind), t.value});
            }
            w.addGridTrackList(tracks);
        }
        for (const IrGridList& list : gc.areaLists()) {
            std::vector<GridArea> areas;
            for (uint32_t i = 0; i < list.count && list.offset + i < gc.areas().size(); ++i) {
                const IrGridArea& a = gc.areas()[list.offset + i];
                areas.push_back(GridArea{a.name, a.rowStart, a.rowEnd, a.colStart, a.colEnd});
            }
            w.addGridAreaList(areas, list.rows, list.cols);
        }
    }

    return w.finish();
}

std::string chunkTagAt(std::span<const uint8_t> bytes, size_t offset) {
    if (offset < sizeof(IrHeader)) {
        return "header";
    }
    size_t pos = sizeof(IrHeader);
    while (pos + sizeof(IrChunkHeader) <= bytes.size()) {
        IrChunkHeader h{};
        std::memcpy(&h, bytes.data() + pos, sizeof(h));
        const size_t end = pos + sizeof(IrChunkHeader) + h.payloadLength;
        if (offset < end) {
            return std::string(h.type, sizeof(h.type));
        }
        pos = end;
    }
    return "?";
}

}  // namespace xt::test

#include "ScheduleFunctions.h"
#include "IROperator.h"
#include "Simplify.h"
#include "Substitute.h"
#include "ExprUsesVar.h"
#include "Var.h"
#include "Qualify.h"
#include "IRMutator.h"
#include "Target.h"
#include "Inline.h"
#include "CodeGen_GPU_Dev.h"
#include "IRPrinter.h"
#include "Func.h"
#include "IREquality.h"
#include "ApplySplits.h"

#include <algorithm>

namespace Halide {
namespace Internal {

using std::string;
using std::map;
using std::vector;
using std::pair;
using std::make_pair;
using std::set;

namespace {
// A structure representing a containing LetStmt, IfThenElse, or For
// loop. Used in build_provide_loop_nest below.
struct Container {
    enum Type {For, Let, If};
    Type type;
    // If it's a for loop, the index in the dims list.
    int dim_idx;
    string name;
    Expr value;
};

bool var_name_match(string candidate, string var) {
    internal_assert(var.find('.') == string::npos)
        << "var_name_match expects unqualified names for the second argument. "
        << "Name passed: " << var << "\n";
    if (candidate == var) return true;
    return Internal::ends_with(candidate, "." + var);
}

} // anonymous namespace

class ContainsImpureCall : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Call *op) {
        if (!op->is_pure()) {
            result = true;
        } else {
            IRVisitor::visit(op);
        }
    }

public:
    bool result = false;
    ContainsImpureCall() {}
};

bool contains_impure_call(const Expr &expr) {
    ContainsImpureCall is_not_pure;
    expr.accept(&is_not_pure);
    return is_not_pure.result;
}

// Build a loop nest about a provide node using a schedule
Stmt build_provide_loop_nest_helper(string func_name,
                                    string prefix,
                                    int start_fuse, // Fuse the dims starting from start_fuse to outermost (if not negative)
                                    const vector<string> &dims, // The pure dims
                                    const vector<Expr> &site,
                                    const vector<Expr> &values,
                                    const vector<Expr> &predicates,
                                    const Schedule &s,
                                    bool is_update) {
    // We'll build it from inside out, starting from a store node,
    // then wrapping it in for loops.

    // Make the (multi-dimensional multi-valued) store node.
    Stmt stmt = Provide::make(func_name, values, site);

    // Add appopriate predicates on the fused loop vars to ensure we don't
    // go out of bounds. Ignore the __outermost dims since it's going to be
    // removed later anyway.
    for (int i = start_fuse; (i >= 0) && (i < (int)s.dims().size()-1); ++i) {
        const Dim &dim = s.dims()[i];
        Expr var = Variable::make(Int(32), prefix + dim.var);
        Expr max = Variable::make(Int(32), prefix + dim.var + ".loop_max");
        Expr min = Variable::make(Int(32), prefix + dim.var + ".loop_min");
        stmt = IfThenElse::make(likely(min <= var), stmt);
        stmt = IfThenElse::make(likely(var <= max), stmt);
    }

    // A map of the dimensions for which we know the extent is a
    // multiple of some Expr. This can happen due to a bound, or
    // align_bounds directive, or if a dim comes from the inside
    // of a split.
    map<string, Expr> dim_extent_alignment;

    // First hunt through the bounds for them.
    for (const Bound &i : s.bounds()) {
        if (i.extent.defined()) {
            dim_extent_alignment[i.var] = i.extent;
        }
        if (i.modulus.defined()) {
            dim_extent_alignment[i.var] = i.modulus;
        }
    }
    // Then use any reduction domain.
    for (const ReductionVariable &i : s.rvars()) {
        dim_extent_alignment[i.var] = i.extent;
    }

    vector<Split> splits = s.splits();

    // Define the function args in terms of the loop variables using the splits
    ApplySplitResult splits_result = apply_splits(splits, is_update, prefix, dim_extent_alignment);
    for (const auto &sub : splits_result.substitutions) {
        stmt = substitute(sub.first, sub.second, stmt);
    }

    // All containing lets and fors. Outermost first.
    vector<Container> nest;

    // Put the desired loop nest into the containers vector.
    for (int i = (int)s.dims().size() - 1; i >= 0; i--) {
        const Dim &dim = s.dims()[i];
        Container c = {Container::For, i, prefix + dim.var, Expr()};
        nest.push_back(c);
    }

    // Put the lets generated from the splits.
    for (int i = splits_result.let_stmts.size() - 1; i >= 0; i--) {
        Container c = {Container::Let, 0, splits_result.let_stmts[i].first,
                       splits_result.let_stmts[i].second};
        nest.push_back(c);
    }

    // Strip off the lets into the containers vector.
    while (const LetStmt *let = stmt.as<LetStmt>()) {
        Container c = {Container::Let, 0, let->name, let->value};
        nest.push_back(c);
        stmt = let->body;
    }

    // Put all the split predicates and the reduction domain predicates into
    // the containers vector.
    int n_predicates = splits_result.predicates.size() + predicates.size();
    for (const auto &pred : splits_result.predicates) {
        Container c = {Container::If, 0, "", pred};
        nest.push_back(c);
    }
    for (Expr pred : predicates) {
        pred = qualify(prefix, pred);
        Container c = {Container::If, 0, "", likely(pred)};
        nest.push_back(c);
    }

    // Resort the containers vector so that lets are as far outwards
    // as possible. Use reverse insertion sort. Start at the first letstmt.
    for (int i = (int)s.dims().size(); i < (int)nest.size() - n_predicates; i++) {
        // Only push up LetStmts.
        internal_assert(nest[i].value.defined());
        internal_assert(nest[i].type == Container::Let);

        for (int j = i-1; j >= 0; j--) {
            // Try to push it up by one.
            internal_assert(nest[j+1].value.defined());
            if (!expr_uses_var(nest[j+1].value, nest[j].name)) {
                std::swap(nest[j+1], nest[j]);
            } else {
                break;
            }
        }
    }

    // Sort the predicate guards so they are as far outwards as possible.
    for (int i = (int)nest.size() - n_predicates; i < (int)nest.size(); i++) {
        // Only push up LetStmts.
        internal_assert(nest[i].value.defined());
        internal_assert(nest[i].type == Container::If);

        // Cannot lift out the predicate guard if it contains call to non-pure function
        if (contains_impure_call(nest[i].value)) {
            continue;
        }

        for (int j = i-1; j >= 0; j--) {
            // Try to push it up by one.
            internal_assert(nest[j+1].value.defined());
            if (!expr_uses_var(nest[j+1].value, nest[j].name)) {
                std::swap(nest[j+1], nest[j]);
            } else {
                break;
            }
        }
    }

    // Rewrap the statement in the containing lets and fors.
    for (int i = (int)nest.size() - 1; i >= 0; i--) {
        if (nest[i].type == Container::Let) {
            internal_assert(nest[i].value.defined());
            stmt = LetStmt::make(nest[i].name, nest[i].value, stmt);
        } else if (nest[i].type == Container::If) {
            internal_assert(nest[i].value.defined());
            stmt = IfThenElse::make(nest[i].value, stmt, Stmt());
        } else {
            internal_assert(nest[i].type == Container::For);
            const Dim &dim = s.dims()[nest[i].dim_idx];
            Expr min = Variable::make(Int(32), nest[i].name + ".loop_min");
            Expr extent = Variable::make(Int(32), nest[i].name + ".loop_extent");
            stmt = For::make(nest[i].name, min, extent, dim.for_type, dim.device_api, stmt);
        }
    }

    // Define the bounds on the split dimensions using the bounds
    // on the function args. If it is a purify, we should use the bounds
    // from the dims instead.
    for (size_t i = splits.size(); i > 0; i--) {
        const Split &split = splits[i-1];

        vector<std::pair<string, Expr>> let_stmts = compute_loop_bounds_after_split(split, prefix);
        for (size_t j = 0; j < let_stmts.size(); j++) {
            stmt = LetStmt::make(let_stmts[j].first, let_stmts[j].second, stmt);
        }
    }

    // Define the bounds on the outermost dummy dimension.
    {
        string o = prefix + Var::outermost().name();
        stmt = LetStmt::make(o + ".loop_min", 0, stmt);
        stmt = LetStmt::make(o + ".loop_max", 0, stmt);
        stmt = LetStmt::make(o + ".loop_extent", 1, stmt);
    }

    // Define the loop mins and extents in terms of the mins and maxs produced by bounds inference
    for (const std::string &i : dims) {
        string var = prefix + i;
        Expr max = Variable::make(Int(32), var + ".max");
        Expr min = Variable::make(Int(32), var + ".min"); // Inject instance name here? (compute instance names during lowering)
        stmt = LetStmt::make(var + ".loop_extent",
                             (max + 1) - min,
                             stmt);
        stmt = LetStmt::make(var + ".loop_min", min, stmt);
        stmt = LetStmt::make(var + ".loop_max", max, stmt);
    }

    // Define the loop mins and extents for the reduction domain (if there is any)
    // in terms of the mins and maxs produced by bounds inference
    for (const ReductionVariable &rv : s.rvars()) {
        string p = prefix + rv.var;
        Expr rmin = Variable::make(Int(32), p + ".min");
        Expr rmax = Variable::make(Int(32), p + ".max");
        stmt = LetStmt::make(p + ".loop_min", rmin, stmt);
        stmt = LetStmt::make(p + ".loop_max", rmax, stmt);
        stmt = LetStmt::make(p + ".loop_extent", rmax - rmin + 1, stmt);
    }

    return stmt;
}

// Build a loop nest about a provide node using a schedule
Stmt build_provide_loop_nest(string func_name,
                             string prefix,
                             int start_fuse,
                             const vector<string> &dims,
                             const Definition &def,
                             bool is_update) {

    internal_assert(!is_update == def.is_init());

    // Default stored values
    vector<Expr> site(def.args().size());
    vector<Expr> values(def.values().size());
    for (size_t i = 0; i < values.size(); i++) {
        Expr v = def.values()[i];
        v = qualify(prefix, v);
        values[i] = v;
        debug(3) << "Value " << i << " = " << v << "\n";
    }

    // Default stored locations
    for (size_t i = 0; i < def.args().size(); i++) {
        Expr s = def.args()[i];
        s = qualify(prefix, s);
        site[i] = s;
        debug(3) << "Site " << i << " = " << s << "\n";
    }

    // Default schedule/values if there is no specialization
    Stmt stmt = build_provide_loop_nest_helper(
        func_name, prefix, start_fuse, dims, site, values,
        def.split_predicate(), def.schedule(), is_update);

    // Make any specialized copies
    const vector<Specialization> &specializations = def.specializations();
    for (size_t i = specializations.size(); i > 0; i--) {
        Expr c = specializations[i-1].condition;
        const Definition &s_def = specializations[i-1].definition;

        Stmt then_case =
            build_provide_loop_nest(func_name, prefix, start_fuse, dims, s_def, is_update);

        stmt = IfThenElse::make(c, then_case, stmt);
    }

    return stmt;
}

// Turn a function into a loop nest that computes it. It will
// refer to external vars of the form function_name.arg_name.min
// and function_name.arg_name.extent to define the bounds over
// which it should be realized. It will compute at least those
// bounds (depending on splits, it may compute more). This loop
// won't do any allocation.
Stmt build_produce(Function f, const Target &target) {

    if (f.has_extern_definition()) {
        // Call the external function

        // Build an argument list
        vector<Expr> extern_call_args;
        const vector<ExternFuncArgument> &args = f.extern_arguments();

        const string &extern_name = f.extern_function_name();

        vector<pair<string, Expr>> lets;

        // Iterate through all of the input args to the extern
        // function building a suitable argument list for the
        // extern function call.
        vector<Expr> buffers_to_annotate;
        vector<Expr> buffers_contents_to_annotate;
        for (const ExternFuncArgument &arg : args) {
            if (arg.is_expr()) {
                extern_call_args.push_back(arg.expr);
            } else if (arg.is_func()) {
                Function input(arg.func);
                for (int k = 0; k < input.outputs(); k++) {
                    string buf_name = input.name();
                    if (input.outputs() > 1) {
                        buf_name += "." + std::to_string(k);
                    }
                    buf_name += ".buffer";
                    Expr buffer = Variable::make(type_of<struct buffer_t *>(), buf_name);
                    extern_call_args.push_back(buffer);
                    buffers_to_annotate.push_back(buffer);
                    buffers_contents_to_annotate.push_back(buffer);
                }
            } else if (arg.is_buffer()) {
                BufferPtr b = arg.buffer;
                Parameter p(b.type(), true, b.dimensions(), b.name());
                p.set_buffer(b);
                string buf_name = b.name() + ".buffer";
                Expr buf = Variable::make(type_of<struct buffer_t *>(), buf_name, p);
                extern_call_args.push_back(buf);
                buffers_to_annotate.push_back(buf);
                buffers_contents_to_annotate.push_back(buf);
            } else if (arg.is_image_param()) {
                Parameter p = arg.image_param;
                string buf_name = p.name() + ".buffer";
                Expr buf = Variable::make(type_of<struct buffer_t *>(), buf_name, p);
                extern_call_args.push_back(buf);
                // Do not annotate ImageParams: both the buffer_t itself,
                // and the contents it points to, should be filled by the caller;
                // if we mark it here, we might mask a missed initialization.
                // buffers_to_annotate.push_back(buf);
                // buffers_contents_to_annotate.push_back(buf);
            } else {
                internal_error << "Bad ExternFuncArgument type\n";
            }
        }

        // Grab the buffer_ts representing the output. If the store
        // level matches the compute level, then we can use the ones
        // already injected by allocation bounds inference. If it's
        // the output to the pipeline then it will similarly be in the
        // symbol table.
        if (f.schedule().store_level() == f.schedule().compute_level()) {
            for (int j = 0; j < f.outputs(); j++) {
                string buf_name = f.name();
                if (f.outputs() > 1) {
                    buf_name += "." + std::to_string(j);
                }
                buf_name += ".buffer";
                Expr buffer = Variable::make(type_of<struct buffer_t *>(), buf_name);
                extern_call_args.push_back(buffer);
                // Since this is a temporary, internal-only buffer, make sure it's marked.
                // (but not the contents! callee is expected to fill that in.)
                buffers_to_annotate.push_back(buffer);
            }
        } else {
            // Store level doesn't match compute level. Make an output
            // buffer just for this subregion.
            string stride_name = f.name();
            if (f.outputs() > 1) {
                stride_name += ".0";
            }
            string stage_name = f.name() + ".s0.";
            const vector<string> f_args = f.args();
            for (int j = 0; j < f.outputs(); j++) {

                vector<Expr> buffer_args(2);

                vector<Expr> top_left;
                for (int k = 0; k < f.dimensions(); k++) {
                    string var = stage_name + f_args[k];
                    top_left.push_back(Variable::make(Int(32), var + ".min"));
                }
                Expr host_ptr = Call::make(f, top_left, j);
                host_ptr = Call::make(Handle(), Call::address_of, {host_ptr}, Call::Intrinsic);

                buffer_args[0] = host_ptr;
                buffer_args[1] = make_zero(f.output_types()[j]);
                for (int k = 0; k < f.dimensions(); k++) {
                    string var = stage_name + f_args[k];
                    Expr min = Variable::make(Int(32), var + ".min");
                    Expr max = Variable::make(Int(32), var + ".max");
                    Expr stride = Variable::make(Int(32), stride_name + ".stride." + std::to_string(k));
                    buffer_args.push_back(min);
                    buffer_args.push_back(max - min + 1);
                    buffer_args.push_back(stride);
                }

                Expr output_buffer_t = Call::make(type_of<struct buffer_t *>(), Call::create_buffer_t,
                                                  buffer_args, Call::Intrinsic);

                string buf_name = f.name() + "." + std::to_string(j) + ".tmp_buffer";
                extern_call_args.push_back(Variable::make(type_of<struct buffer_t *>(), buf_name));
                // Since this is a temporary, internal-only buffer, make sure it's marked.
                // (but not the contents! callee is expected to fill that in.)
                buffers_to_annotate.push_back(extern_call_args.back());
                lets.push_back(make_pair(buf_name, output_buffer_t));
            }
        }

        Stmt annotate;
        if (target.has_feature(Target::MSAN)) {
            // Mark the buffers as initialized before calling out.
            for (const auto &buffer: buffers_to_annotate) {
                // Return type is really 'void', but no way to represent that in our IR.
                // Precedent (from halide_print, etc) is to use Int(32) and ignore the result.
                Expr sizeof_buffer_t((uint64_t) sizeof(buffer_t));
                Stmt mark_buffer = Evaluate::make(Call::make(Int(32), "halide_msan_annotate_memory_is_initialized", {buffer, sizeof_buffer_t}, Call::Extern));
                if (annotate.defined()) {
                    annotate = Block::make(annotate, mark_buffer);
                } else {
                    annotate = mark_buffer;
                }
            }
            for (const auto &buffer: buffers_contents_to_annotate) {
                // Return type is really 'void', but no way to represent that in our IR.
                // Precedent (from halide_print, etc) is to use Int(32) and ignore the result.
                Stmt mark_contents = Evaluate::make(Call::make(Int(32), "halide_msan_annotate_buffer_is_initialized", {buffer}, Call::Extern));
                if (annotate.defined()) {
                    annotate = Block::make(annotate, mark_contents);
                } else {
                    annotate = mark_contents;
                }
            }
        }

        // Make the extern call
        Expr e = Call::make(Int(32), extern_name, extern_call_args,
                            f.extern_definition_is_c_plus_plus() ? Call::ExternCPlusPlus
                                                                 : Call::Extern);
        string result_name = unique_name('t');
        Expr result = Variable::make(Int(32), result_name);
        // Check if it succeeded
        Expr error = Call::make(Int(32), "halide_error_extern_stage_failed",
                                {extern_name, result}, Call::Extern);
        Stmt check = AssertStmt::make(EQ::make(result, 0), error);
        check = LetStmt::make(result_name, e, check);

        for (size_t i = 0; i < lets.size(); i++) {
            check = LetStmt::make(lets[i].first, lets[i].second, check);
        }

        if (annotate.defined()) {
            check = Block::make(annotate, check);
        }
        return check;
    } else {

        string prefix = f.name() + ".s0.";
        vector<string> dims = f.args();
        return build_provide_loop_nest(f.name(), prefix, -1, dims, f.definition(), false);
    }
}

// Build the loop nests that update a function (assuming it's a reduction).
vector<Stmt> build_update(Function f) {

    vector<Stmt> updates;

    for (size_t i = 0; i < f.updates().size(); i++) {
        const Definition &def = f.update(i);

        string prefix = f.name() + ".s" + std::to_string(i+1) + ".";

        vector<string> dims = f.args();
        Stmt loop = build_provide_loop_nest(f.name(), prefix, -1, dims, def, true);
        updates.push_back(loop);
    }

    return updates;
}

pair<Stmt, Stmt> build_production(Function func, const Target &target) {
    Stmt produce = build_produce(func, target);
    vector<Stmt> updates = build_update(func);

    // Combine the update steps
    Stmt merged_updates = Block::make(updates);
    return make_pair(produce, merged_updates);
}

// A schedule may include explicit bounds on some dimension. This
// injects assertions that check that those bounds are sufficiently
// large to cover the inferred bounds required.
Stmt inject_explicit_bounds(Stmt body, Function func) {
    const Schedule &s = func.schedule();
    for (size_t stage = 0; stage <= func.updates().size(); stage++) {
        for (size_t i = 0; i < s.bounds().size(); i++) {
            Bound b = s.bounds()[i];
            string prefix = func.name() + ".s" + std::to_string(stage) + "." + b.var;
            string min_name = prefix + ".min_unbounded";
            string max_name = prefix + ".max_unbounded";
            Expr min_var = Variable::make(Int(32), min_name);
            Expr max_var = Variable::make(Int(32), max_name);
            if (!b.min.defined()) {
                b.min = min_var;
            }
            if (!b.extent.defined()) {
                // This is just a bounds alignment, which always expands the region computed.
                continue;
            }

            Expr max_val = (b.extent + b.min) - 1;
            Expr min_val = b.min;

            Expr check = (min_val <= min_var) && (max_val >= max_var);
            Expr error_msg = Call::make(Int(32), "halide_error_explicit_bounds_too_small",
                                        {b.var, func.name(), min_val, max_val, min_var, max_var},
                                        Call::Extern);
            body = Block::make(AssertStmt::make(check, error_msg), body);
        }
    }

    return body;
}

class IsUsedInStmt : public IRVisitor {
    string func;

    using IRVisitor::visit;

    void visit(const Call *op) {
        IRVisitor::visit(op);
        if (op->name == func) result = true;
    }

    // A reference to the function's buffers counts as a use
    void visit(const Variable *op) {
        if (op->type.is_handle() &&
            starts_with(op->name, func + ".") &&
            ends_with(op->name, ".buffer")) {
            result = true;
        }
    }

public:
    bool result;
    IsUsedInStmt(Function f) : func(f.name()), result(false) {
    }

};

bool function_is_used_in_stmt(Function f, Stmt s) {
    IsUsedInStmt is_called(f);
    s.accept(&is_called);
    return is_called.result;
}

class IsRealizedInStmt : public IRVisitor {
    string func;

    using IRVisitor::visit;

    void visit(const Realize *op) {
        IRVisitor::visit(op);
        if (op->name == func) result = true;
    }

public:
    bool result;
    IsRealizedInStmt(Function f) : func(f.name()), result(false) {}
};

// Check if function 'f' is already realized in Stmt 's'.
bool function_is_already_realized_in_stmt(Function f, Stmt s) {
    IsRealizedInStmt is_realized(f);
    s.accept(&is_realized);
    return is_realized.result;
}

// Inject the allocation and realization of a function (which are not part of any
// fused group) into an existing loop nest using its schedule
class InjectRealization : public IRMutator {
public:
    const Function &func;
    bool is_output, found_store_level, found_compute_level;
    const Target &target;
    const map<string, Function> &env;

    InjectRealization(const Function &f, bool o, const Target &t, const map<string, Function> &env) :
            func(f), is_output(o),
            found_store_level(false), found_compute_level(false),
            target(t), env(env) {
    }

    // Determine if 'loop_name' is the right level to inject produce/realize node
    // of 'func'. If 'loop_name' is a fused group, we should inject it at the
    // fused parent loop of the group.
    bool is_the_right_level(const string &loop_name) {
        if (loop_name == LoopLevel::root().to_string()) {
            return true;
        }

        vector<string> v = split_string(loop_name, ".");
        internal_assert(v.size() > 2);
        string func_name = v[0];
        string var = v[v.size()-1];

        int stage = -1;
        for (size_t i = 1; i < v.size() - 1; ++i) {
            if (v[i].substr(0, 1) == "s") {
                string str = v[i].substr(1, v[i].size() - 1);
                bool has_only_digits = (str.find_first_not_of( "0123456789" ) == string::npos);
                if (has_only_digits) {
                    stage = atoi(str.c_str());
                }
            }
        }
        internal_assert(stage >= 0);

        const auto it = env.find(func_name);
        internal_assert(it != env.end());
        const Function &f = it->second;
        internal_assert(stage <= (int)f.updates().size());

        const Definition &def = (stage == 0) ? f.definition() : f.update(stage - 1);
        const LoopLevel &fuse_level = def.schedule().fuse_level().level;
        if (fuse_level.is_inline() || fuse_level.is_root()) {
            // It isn't fused to anyone
            return true;
        } else {
            // Need to find out if it is fused at 'var'
            const vector<Dim> &dims = def.schedule().dims();
            const auto it1 = std::find_if(dims.begin(), dims.end(),
                [&fuse_level](const Dim& d) { return var_name_match(d.var, fuse_level.var().name()); });
            internal_assert(it1 != dims.end());

            const auto it2 = std::find_if(dims.begin(), dims.end(),
                [&var](const Dim& d) { return var_name_match(d.var, var); });
            internal_assert(it2 != dims.end());

            return it2 < it1;
        }
        return false;
    }

private:

    Stmt build_pipeline(Stmt s) {
        pair<Stmt, Stmt> realization = build_production(func, target);

        Stmt producer;
        if (realization.first.defined() && realization.second.defined()) {
            producer = Block::make(realization.first, realization.second);
        } else if (realization.first.defined()) {
            producer = realization.first;
        } else {
            internal_assert(realization.second.defined());
            producer = realization.second;
        }
        producer = ProducerConsumer::make(func.name(), true, producer);
        Stmt consumer = ProducerConsumer::make(func.name(), false, s);

        return Block::make(producer, consumer);
    }

    Stmt build_realize(Stmt s) {
        if (!is_output) {
            Region bounds;
            string name = func.name();
            const vector<string> func_args = func.args();
            for (int i = 0; i < func.dimensions(); i++) {
                const string &arg = func_args[i];
                Expr min = Variable::make(Int(32), name + "." + arg + ".min_realized");
                Expr extent = Variable::make(Int(32), name + "." + arg + ".extent_realized");
                bounds.push_back(Range(min, extent));
            }

            s = Realize::make(name, func.output_types(), bounds, const_true(), s);
        }

        // This is also the point at which we inject explicit bounds
        // for this realization.
        if (target.has_feature(Target::NoAsserts)) {
            return s;
        } else {
            return inject_explicit_bounds(s, func);
        }
    }

    using IRMutator::visit;

    void visit(const For *for_loop) {
        debug(3) << "InjectRealization of " << func.name() << " entering for loop over " << for_loop->name << "\n";
        const LoopLevel &compute_level = func.schedule().compute_level();
        const LoopLevel &store_level = func.schedule().store_level();

        Stmt body = for_loop->body;

        // Dig through any let statements
        vector<pair<string, Expr>> lets;
        while (const LetStmt *l = body.as<LetStmt>()) {
            lets.push_back(make_pair(l->name, l->value));
            body = l->body;
        }

        // Can't schedule extern things inside a vector for loop
        if (func.has_extern_definition() &&
            func.schedule().compute_level().is_inline() &&
            for_loop->for_type == ForType::Vectorized &&
            !function_is_already_realized_in_stmt(func, for_loop) &&
            function_is_used_in_stmt(func, for_loop)) {

            // If we're trying to inline an extern function, schedule it here and bail out
            debug(2) << "Injecting realization of " << func.name() << " around node " << Stmt(for_loop) << "\n";
            stmt = build_realize(build_pipeline(for_loop));
            found_store_level = found_compute_level = true;
            return;
        }

        body = mutate(body);

        if (compute_level.match(for_loop->name) && is_the_right_level(for_loop->name)) {
            debug(3) << "Found compute level\n";
            if (!function_is_already_realized_in_stmt(func, body) &&
                (function_is_used_in_stmt(func, body) || is_output)) {
                body = build_pipeline(body);
            }
            found_compute_level = true;
        }

        if (store_level.match(for_loop->name) && is_the_right_level(for_loop->name)) {
            debug(3) << "Found store level\n";
            internal_assert(found_compute_level)
                << "The compute loop level was not found within the store loop level!\n";

            if (!function_is_already_realized_in_stmt(func, body) &&
                (function_is_used_in_stmt(func, body) || is_output)) {
                body = build_realize(body);
            }

            found_store_level = true;
        }

        // Reinstate the let statements
        for (size_t i = lets.size(); i > 0; i--) {
            body = LetStmt::make(lets[i - 1].first, lets[i - 1].second, body);
        }

        if (body.same_as(for_loop->body)) {
            stmt = for_loop;
        } else {
            stmt = For::make(for_loop->name,
                             for_loop->min,
                             for_loop->extent,
                             for_loop->for_type,
                             for_loop->device_api,
                             body);
        }
    }

    // If we're an inline update or extern, we may need to inject a realization here
    virtual void visit(const Provide *op) {
        if (op->name != func.name() &&
            !func.is_pure() &&
            func.schedule().compute_level().is_inline() &&
            function_is_used_in_stmt(func, op)) {

            // Prefix all calls to func in op
            stmt = build_realize(build_pipeline(op));
            found_store_level = found_compute_level = true;
        } else {
            stmt = op;
        }
    }
};

std::ostream& operator<<(std::ostream& out, const std::vector<Function>& v) {
    out << "{ ";
    for (size_t i = 0; i < v.size(); ++i) {
        out << v[i].name();
        if (i != v.size() - 1) {
            out << ", ";
        }
    }
    out << " }";
    return out;
}

class InjectStmt : public IRMutator {
public:
    Stmt injected_stmt;
    bool found_level;
    LoopLevel level;

    InjectStmt(const Stmt &s, const LoopLevel &level)
        : injected_stmt(s), found_level(false), level(level) {}

private:
    using IRMutator::visit;

    void visit(const For *for_loop) {
        Stmt body = mutate(for_loop->body);

        if (level.match(for_loop->name)) {
            body = Block::make(body, injected_stmt);
            found_level = true;
        }

        if (body.same_as(for_loop->body)) {
            stmt = for_loop;
        } else {
            stmt = For::make(for_loop->name,
                             for_loop->min,
                             for_loop->extent,
                             for_loop->for_type,
                             for_loop->device_api,
                             body);
        }
    }
};

// Inject 'injected' into 'root' at 'level'.
Stmt inject_stmt(Stmt root, Stmt injected, const LoopLevel &level) {
    if (!root.defined()) {
        return injected;
    }
    if (!injected.defined()) {
        return root;
    }
    if (level.is_inline() || level.is_root()) {
        return Block::make(root, injected);
    }
    InjectStmt injector(injected, level);
    root = injector.mutate(root);
    internal_assert(injector.found_level);
    return root;
}

class SubstituteBounds : public IRMutator {
public:
    map<string, Expr> &bounds;
    const map<string, Expr> &replacements;
    SubstituteBounds(map<string, Expr> &b, const map<string, Expr> &r) : bounds(b), replacements(r) {}

private:
    using IRMutator::visit;

    void visit(const LetStmt *op) {
        auto iter = bounds.find(op->name);
        if (iter != bounds.end()) {
            iter->second = op->value;
        }
        IRMutator::visit(op);
    }

    void visit(const For *op) {
        const Variable *min_var = op->min.as<Variable>();
        const Variable *extent_var = op->extent.as<Variable>();
        if (min_var && extent_var) {
            Expr min_val, extent_val;
            {
                const auto it = replacements.find(min_var->name);
                if (it != replacements.end()) {
                    min_val = it->second;
                }
            }
            {
                const auto it = replacements.find(extent_var->name);
                if (it != replacements.end()) {
                    extent_val = it->second;
                }
            }

            if (!min_val.defined()|| !extent_val.defined()) {
                IRMutator::visit(op);
                return;
            }

            Stmt body = mutate(op->body);

            size_t last_dot = op->name.rfind('.');
            internal_assert(last_dot != string::npos);
            string new_var = op->name.substr(0, last_dot) + ".fused." + op->name.substr(last_dot + 1);

            // If this is the child fused loop, might as well clear the for-loop
            // scheduling flag (parallel, vectorize, or unrolled) since it is of
            // extent one anyway.
            ForType for_type = op->for_type;
            if (is_one(extent_val)) {
                for_type = ForType::Serial;
            }

            stmt = For::make(new_var, Variable::make(Int(32), new_var + ".loop_min"),
                             Variable::make(Int(32), new_var + ".loop_extent"),
                             for_type, op->device_api, body);

            // Add let stmts defining the bound of the renamed for-loop.
            stmt = LetStmt::make(new_var + ".loop_min", min_val, stmt);
            stmt = LetStmt::make(new_var + ".loop_max", simplify(min_val + extent_val - 1), stmt);
            stmt = LetStmt::make(new_var + ".loop_extent", extent_val, stmt);
            // Replace any reference to the old loop name with the new one.
            stmt = substitute(op->name, Variable::make(Int(32), new_var), stmt);
        } else {
            IRMutator::visit(op);
        }
    }
};

// The bounds of every loop exist in 'replacements' should be replaced. The
// loop is also renamed by adding ".fused" in the original name before the
// variable name. We also replaced the value of element in 'bounds' when we find
// let stmt with the same name in 's'.
Stmt substitute_bounds(Stmt s, map<string, Expr> &bounds, const map<string, Expr> &replacements) {
    if (!s.defined()) {
        return s;
    }
    SubstituteBounds subs(bounds, replacements);
    s = subs.mutate(s);
    return s;
}

// Inject the allocation and realization of a group of functions which are
// to be fused into an existing loop nest using its schedule.
class InjectGroupRealization : public IRMutator {
public:
    vector<Function> &group; // Member of the fused loop starting from the first to be realized to the last
    const vector<bool> &is_output_list; // List of booleans indicating if group[i] is an output
    bool found_store_level, found_compute_level;
    const Target &target;
    LoopLevel compute_level;
    LoopLevel store_level;
    const map<string, Function> &env;

    InjectGroupRealization(vector<Function> &g, const vector<bool> &o, const Target &t,
                           const vector<string> &order, const map<string, Function> &env)
            : group(g), is_output_list(o), found_store_level(false), found_compute_level(false), target(t), env(env) {
        internal_assert(!group.empty());
        internal_assert(group.size() == is_output_list.size());

        compute_level = group[0].schedule().compute_level();
        store_level = group[0].schedule().store_level();
        internal_assert(!compute_level.is_inline());
    }

private:

    Stmt build_pipeline_group(Stmt s) {
        map<string, bool> skip;
        size_t num_skipped = 0;
        for (size_t i = 0; i < group.size(); ++i) {
            if (function_is_used_in_stmt(group[i], s) || is_output_list[i]) {
                skip[group[i].name()] = false;
            } else {
                skip[group[i].name()] = true;
                num_skipped += 1;
            }
        }

        if (num_skipped == group.size()) {
            // All producers are skipped.
            return s;
        }

        user_assert(!skip[group[0].name()])
            << "Invalid compute_with: the 'parent' function " << group[0].name()
            << " in fused group " << group << " is not used at the compute_at level "
            << compute_level.to_string() << ".\n";

        // Add the consumer nodes.
        Stmt consume = s;
        for (size_t i = group.size(); i > 0; --i) {
            if (!skip[group[i-1].name()]) {
                consume = ProducerConsumer::make(group[i-1].name(), false, consume);
            }
        }

        // Build the loops.
        map<string, Expr> bounds; // The original bounds of the loopness (without any loop-fusion)
        map<string, Expr> replacements;

        int parent_index = -1; // The first function in the group that is not skipped
        vector<pair<string, Expr>> add_lets;
        Stmt produce;
        for (size_t i = 0; i < group.size(); ++i) {
            if (!skip[group[i].name()]) {
                produce = build_produce(skip, group[i], produce, bounds, replacements, add_lets);
                if (parent_index == -1) {
                    parent_index = i;
                }
            }
        }
        internal_assert((parent_index >= 0) && (parent_index < (int)group.size()));

        // Rewrap the loop in the containing lets.
        for (size_t i = add_lets.size(); i > 0; --i) {
            const auto &b = add_lets[i-1];
            produce = LetStmt::make(b.first, b.second, produce);
        }

        // Replace all the child fused loops with the appropriate bounds (i.e.
        // the min/max should refer to the parent loop vars and the extent should
        // be one).
        produce = substitute_bounds(produce, bounds, replacements);

        // Replace the bounds of parent fused loops with union of bound of
        // the fused loops.
        produce = replace_parent_bound_with_union_bound(skip, group[parent_index], produce, bounds);

        // Add the producer nodes.
        for (size_t i = group.size(); i > 0; --i) {
            if (!skip[group[i-1].name()]) {
                produce = ProducerConsumer::make(group[i-1].name(), true, produce);
            }
        }

        // Shifts the loops according to the alignment strategies.


        return Block::make(produce, consume);
    }

    Stmt build_produce(const map<string, bool> &skip, Function f, Stmt produce, map<string, Expr> &bounds,
                       map<string, Expr> &replacements, vector<pair<string, Expr>> &add_lets) {
        string prefix = f.name() + ".s0.";
        produce = inject_stmt(produce,
                              build_produce_definition(skip, f, prefix, f.definition(),
                                                       false, bounds, replacements, add_lets),
                              f.definition().schedule().fuse_level().level);

        for (size_t j = 0; j < f.updates().size(); ++j) {
            const Definition &def = f.updates()[j];
            string prefix = f.name() + ".s" + std::to_string(j+1) + ".";
            produce = inject_stmt(produce,
                                  build_produce_definition(skip, f, prefix, def, true,
                                                           bounds, replacements, add_lets),
                                  def.schedule().fuse_level().level);
        }
        return produce;
    }

    Stmt build_produce_definition(const map<string, bool> &skip, Function f,
                                  const string &prefix, const Definition &def,
                                  bool is_update, map<string, Expr> &bounds,
                                  map<string, Expr> &replacements,
                                  vector<pair<string, Expr>> &add_lets) {
        const vector<Dim> &dims = def.schedule().dims(); // From inner to outer
        const LoopLevel &fuse_level = def.schedule().fuse_level().level;

        size_t start_fuse = dims.size();
        if (!fuse_level.is_inline() && !fuse_level.is_root()) {
            if (!skip.find(fuse_level.func().name())->second) {
                const auto iter = std::find_if(dims.begin(), dims.end(),
                    [&fuse_level](const Dim& d) { return var_name_match(d.var, fuse_level.var().name()); });
                internal_assert(iter != dims.end());
                start_fuse = iter - dims.begin();
            }
        }

        // The bounds of the child fused loops should be replaced to refer to the
        // parent fused loops. Here, we are only collecting the ones we should
        // replace. The actual replacement is done later.
        for (const FusedPair &pair : def.schedule().fused_pairs()) {
            if (env.find(pair.func_2) == env.end()) {
                continue;
            }
            if (skip.find(pair.func_2)->second) {
                continue;
            }
            const auto iter = std::find_if(dims.begin(), dims.end(),
                [&pair](const Dim& d) { return var_name_match(d.var, pair.var_name); });
            internal_assert(iter != dims.end());
            start_fuse = std::min(start_fuse, (size_t)(iter - dims.begin()));
            // Should ignore the __outermost dummy dimension.
            for (size_t i = iter - dims.begin(); i < dims.size() - 1; ++i) {
                string var = pair.func_2 + ".s" + std::to_string(pair.stage_2) + "." + dims[i].var;
                bounds.emplace(var + ".loop_min", Expr());
                bounds.emplace(var + ".loop_max", Expr());
                bounds.emplace(var + ".loop_extent", Expr());

                string var_orig = pair.func_1 + ".s" + std::to_string(pair.stage_1) + "." + dims[i].var;
                Expr val = Variable::make(Int(32), var_orig);
                replacements.emplace(var + ".loop_min", val);
                replacements.emplace(var + ".loop_max", val);
                replacements.emplace(var + ".loop_extent", make_const(Int(32), 1));

                bounds.emplace(var_orig + ".loop_min", Expr());
                bounds.emplace(var_orig + ".loop_max", Expr());
                bounds.emplace(var_orig + ".loop_extent", Expr());
            }
        }

        const vector<string> f_args = f.args();
        Stmt produce = build_provide_loop_nest(f.name(), prefix, start_fuse, f_args, def, is_update);

        // Strip off the containing lets. The bounds of the parent fused loops
        // (i.e. the union bounds) might refer to them, so we need to move them
        // to the topmost position.
        while (const LetStmt *let = produce.as<LetStmt>()) {
            add_lets.push_back(std::make_pair(let->name, let->value));
            produce = let->body;
        }
        return produce;
    }

    void collect_all_dependence_helper(const map<string, bool> &skip, const string &prefix,
                                       const Definition &def, const FusedPair &p,
                                       vector<FusedPair> &dependence, set<string> &visited) {
        visited.insert(prefix);
        dependence.push_back(p);
        for (const FusedPair &pair : def.schedule().fused_pairs()) {
            if (skip.find(pair.func_2)->second) {
                continue;
            }
            const auto iter = env.find(pair.func_2);
            if (iter == env.end()) {
                continue;
            }
            const Function &f = iter->second;
            string prefix_2 = pair.func_2 + ".s" + std::to_string(pair.stage_2) + "." + pair.var_name;
            if (visited.find(prefix_2) == visited.end()) {
                const Definition &def_2 = (pair.stage_2 == 0) ? f.definition() : f.update(pair.stage_2 - 1);
                collect_all_dependence_helper(skip, prefix_2, def_2, pair, dependence, visited);
            }
        }
    }

    // Collect all fused pairs that directly/indirectly related to 'def'
    vector<FusedPair> collect_all_dependence(const map<string, bool> &skip, const Definition &def) {
        set<string> visited;
        vector<FusedPair> dependence;

        for (const FusedPair &pair : def.schedule().fused_pairs()) {
            if (skip.find(pair.func_2)->second) {
                continue;
            }
            const auto iter = env.find(pair.func_2);
            if (iter == env.end()) {
                continue;
            }
            const Function &f = iter->second;
            string prefix = pair.func_2 + ".s" + std::to_string(pair.stage_2) + "." + pair.var_name;
            if (visited.find(prefix) == visited.end()) {
                const Definition &def_2 = (pair.stage_2 == 0) ? f.definition() : f.update(pair.stage_2 - 1);
                collect_all_dependence_helper(skip, prefix, def_2, pair, dependence, visited);
            }
        }
        return dependence;
    }

    // Replace the bounds of the parent fused loop (i.e. the first one to be
    // realized in the group) with union of the bounds of the fused group.
    Stmt replace_parent_bound_with_union_bound(const map<string, bool> &skip, Function f,
                                               Stmt produce, const map<string, Expr> &bounds) {
        string prefix = f.name() + ".s0";
        const Definition &def = f.definition();
        const vector<Dim> &dims = def.schedule().dims(); // From inner to outer

        map<string, Expr> replacements;

        vector<FusedPair> dependence = collect_all_dependence(skip, def);

        // Compute the union of the bounds of the fused loops.
        for (const FusedPair &pair : dependence) {
            if (skip.find(pair.func_2)->second) {
                continue;
            }
            const auto iter = std::find_if(dims.begin(), dims.end(),
                [&pair](const Dim& d) { return var_name_match(d.var, pair.var_name); });
            internal_assert(iter != dims.end());
            // Should ignore the __outermost dummy dimension.
            for (size_t i = iter - dims.begin(); i < dims.size() - 1; ++i) {
                string var_2 = pair.func_2 + ".s" + std::to_string(pair.stage_2) + "." + dims[i].var;
                internal_assert(bounds.count(var_2 + ".loop_min"));
                internal_assert(bounds.count(var_2 + ".loop_max"));
                internal_assert(bounds.count(var_2 + ".loop_extent"));
                Expr min_2 = bounds.find(var_2 + ".loop_min")->second;
                Expr max_2 = bounds.find(var_2 + ".loop_max")->second;
                Expr extent_2 = bounds.find(var_2 + ".loop_extent")->second;

                string var_1 = prefix + "." + dims[i].var;
                internal_assert(bounds.count(var_1 + ".loop_min"));
                internal_assert(bounds.count(var_1 + ".loop_max"));
                internal_assert(bounds.count(var_1 + ".loop_extent"));

                Expr min_1, max_1, extent_1;
                const auto it = replacements.find(var_1 + ".loop_min");
                if (it == replacements.end()) {
                    min_1 = bounds.find(var_1 + ".loop_min")->second;
                    max_1 = bounds.find(var_1 + ".loop_max")->second;
                    extent_1 = bounds.find(var_1 + ".loop_extent")->second;
                } else {
                    min_1 = replacements[var_1 + ".loop_min"];
                    max_1 = replacements[var_1 + ".loop_max"];
                    extent_1 = replacements[var_1 + ".loop_extent"];
                }

                replacements[var_1 + ".loop_min"] = simplify(min(min_1, min_2));
                replacements[var_1 + ".loop_max"] = simplify(max(max_1, max_2));
                replacements[var_1 + ".loop_extent"] =
                    simplify((replacements[var_1 + ".loop_max"] + 1) - replacements[var_1 + ".loop_min"]);
            }
        }

        // Now, replace the bounds of the parent fused loops with the union bounds.
        map<string, Expr> empty_bounds;
        produce = substitute_bounds(produce, empty_bounds, replacements);
        return produce;
    }

    Stmt build_realize_group(Stmt s) {
        for (size_t i = group.size(); i > 0; --i) {
            if (function_is_used_in_stmt(group[i-1], s) || is_output_list[i-1]) {
                s = build_realize(s, group[i-1], is_output_list[i-1]);
            }
        }
        return s;
    }

    Stmt build_realize(Stmt s, Function func, bool is_output) {
        if (!is_output) {
            Region bounds;
            string name = func.name();
            const vector<string> func_args = func.args();
            for (int i = 0; i < func.dimensions(); i++) {
                const string &arg = func_args[i];
                Expr min = Variable::make(Int(32), name + "." + arg + ".min_realized");
                Expr extent = Variable::make(Int(32), name + "." + arg + ".extent_realized");
                bounds.push_back(Range(min, extent));
            }

            s = Realize::make(name, func.output_types(), bounds, const_true(), s);
        }

        // This is also the point at which we inject explicit bounds
        // for this realization.
        if (target.has_feature(Target::NoAsserts)) {
            return s;
        } else {
            return inject_explicit_bounds(s, func);
        }
    }

    using IRMutator::visit;

    void visit(const For *for_loop) {
        debug(3) << "InjectGroupRealization of " << group << " entering for loop over " << for_loop->name << "\n";

        Stmt body = for_loop->body;

        // Dig through any let statements
        vector<pair<string, Expr>> lets;
        while (const LetStmt *l = body.as<LetStmt>()) {
            lets.push_back(make_pair(l->name, l->value));
            body = l->body;
        }

        body = mutate(body);

        if (compute_level.match(for_loop->name)) {
            debug(3) << "Found compute level at " << for_loop->name << "\n";
            body = build_pipeline_group(body);
            found_compute_level = true;
        }

        if (store_level.match(for_loop->name)) {
            debug(3) << "Found store level at " << for_loop->name << "\n";
            internal_assert(found_compute_level)
                << "The compute loop level was not found within the store loop level!\n";
            body = build_realize_group(body);
            found_store_level = true;
        }

        // Reinstate the let statements
        for (size_t i = lets.size(); i > 0; i--) {
            body = LetStmt::make(lets[i - 1].first, lets[i - 1].second, body);
        }

        if (body.same_as(for_loop->body)) {
            stmt = for_loop;
        } else {
            stmt = For::make(for_loop->name,
                             for_loop->min,
                             for_loop->extent,
                             for_loop->for_type,
                             for_loop->device_api,
                             body);
        }
    }
};


class ComputeLegalSchedules : public IRVisitor {
public:
    struct Site {
        bool is_parallel;
        LoopLevel loop_level;
    };
    vector<Site> sites_allowed;

    ComputeLegalSchedules(Function f, const map<string, Function> &env) : func(f), found(false), env(env) {}

private:
    using IRVisitor::visit;

    vector<Site> sites;
    Function func;
    bool found;
    const map<string, Function> &env;

    void visit(const For *f) {
        f->min.accept(this);
        f->extent.accept(this);
        size_t first_dot = f->name.find('.');
        size_t last_dot = f->name.rfind('.');
        internal_assert(first_dot != string::npos && last_dot != string::npos);
        string func = f->name.substr(0, first_dot);
        string var = f->name.substr(last_dot + 1);
        LoopLevel loop_level;
        if (func.empty()) {
            internal_assert(!var.empty());
            loop_level = LoopLevel::root();
        } else {
            auto it = env.find(func);
            internal_assert(it != env.end()) << "Unable to find Function " << func << " in env (Var = " << var << ")\n";
            loop_level = LoopLevel(it->second, Var(var));
        }
        Site s = {f->for_type == ForType::Parallel ||
                  f->for_type == ForType::Vectorized,
                  loop_level};
        sites.push_back(s);
        f->body.accept(this);
        sites.pop_back();
    }

    void register_use() {
        if (!found) {
            found = true;
            sites_allowed = sites;
        } else {
            vector<Site> common_sites;

            // Take the common sites between sites and sites_allowed
            for (const Site &s1 : sites) {
                for (const Site &s2 : sites_allowed) {
                    if (s1.loop_level.match(s2.loop_level)) {
                        common_sites.push_back(s1);
                        break;
                    }
                }
            }

            sites_allowed.swap(common_sites);
        }
    }

    void visit(const Call *c) {
        IRVisitor::visit(c);

        if (c->name == func.name()) {
            register_use();
        }
    }

    void visit(const Variable *v) {
        if (v->type.is_handle() &&
            starts_with(v->name, func.name() + ".") &&
            ends_with(v->name, ".buffer")) {
            register_use();
        }
    }
};

string schedule_to_source(Function f,
                          LoopLevel store_at,
                          LoopLevel compute_at) {
    std::ostringstream ss;
    ss << f.name();
    if (compute_at.is_inline()) {
        ss << ".compute_inline()";
    } else {
        if (!store_at.match(compute_at)) {
            if (store_at.is_root()) {
                ss << ".store_root()";
            } else {
                string store_var_name = store_at.var().name();
                if (store_var_name == Var::outermost().name()) {
                    store_var_name = "Var::outermost()";
                }
                ss << ".store_at(" << store_at.func().name() << ", " << store_var_name << ")";
            }
        }
        if (compute_at.is_root()) {
            ss << ".compute_root()";
        } else {
            string compute_var_name = compute_at.var().name();
            if (compute_var_name == Var::outermost().name()) {
                compute_var_name = "Var::outermost()";
            }
            ss << ".compute_at(" << compute_at.func().name() << ", " << compute_var_name << ")";
        }
    }
    ss << ";";
    return ss.str();
}

class StmtUsesFunc : public IRVisitor {
    using IRVisitor::visit;
    string func;
    void visit(const Call *op) {
        if (op->name == func) {
            result = true;
        }
        IRVisitor::visit(op);
    }
public:
    bool result = false;
    StmtUsesFunc(string f) : func(f) {}
};

class PrintUsesOfFunc : public IRVisitor {
    using IRVisitor::visit;

    int indent = 1;
    string func, caller;
    bool last_print_was_ellipsis = false;
    std::ostream &stream;

    void do_indent() {
        for (int i = 0; i < indent; i++) {
            stream << "  ";
        }
    }

    void visit(const For *op) {
        if (ends_with(op->name, Var::outermost().name()) ||
            ends_with(op->name, LoopLevel::root().to_string())) {
            IRVisitor::visit(op);
        } else {

            int old_indent = indent;

            StmtUsesFunc uses(func);
            op->body.accept(&uses);
            if (!uses.result) {
                if (!last_print_was_ellipsis) {
                    do_indent();
                    stream << "...\n";
                    last_print_was_ellipsis = true;
                }
            } else {
                do_indent();
                stream << "for " << op->name << ":\n";
                last_print_was_ellipsis = false;
                indent++;
            }

            IRVisitor::visit(op);
            indent = old_indent;
        }
    }

    void visit(const ProducerConsumer *op) {
        if (op->is_producer) {
            string old_caller = caller;
            caller = op->name;
            op->body.accept(this);
            caller = old_caller;
        } else {
            IRVisitor::visit(op);
        }
    }

    void visit(const Call *op) {
        if (op->name == func) {
            do_indent();
            stream << caller << " uses " << func << "\n";
            last_print_was_ellipsis = false;
        } else {
            IRVisitor::visit(op);
        }
    }

public:
    PrintUsesOfFunc(string f, std::ostream &s) : func(f), stream(s) {}
};

void validate_schedule(Function f, Stmt s, const Target &target, bool is_output, const map<string, Function> &env) {

    // If f is extern, check that none of its inputs are scheduled inline.
    if (f.has_extern_definition()) {
        for (const ExternFuncArgument &arg : f.extern_arguments()) {
            if (arg.is_func()) {
                Function g(arg.func);
                if (g.schedule().compute_level().is_inline()) {
                    user_error
                        << "Func " << g.name() << " cannot be scheduled to be computed inline, "
                        << "because it is used in the externally-computed function " << f.name() << "\n";
                }
            }
        }
    }

    // Emit a warning if only some of the steps have been scheduled.
    bool any_scheduled = f.schedule().touched();
    for (const Definition &r : f.updates()) {
        any_scheduled = any_scheduled || r.schedule().touched();
    }
    if (any_scheduled) {
        for (size_t i = 0; i < f.updates().size(); i++) {
            const Definition &r = f.update(i);
            if (!r.schedule().touched()) {
                user_warning << "Warning: Update step " << i
                             << " of function " << f.name()
                             << " has not been scheduled, even though some other"
                             << " steps have been. You may have forgotten to"
                             << " schedule it. If this was intentional, call "
                             << f.name() << ".update(" << i << ") to suppress"
                             << " this warning.\n";
            }
        }
    }

    // If the func is scheduled on the gpu, check that the relevant
    // api is enabled in the target.
    vector<Definition> definitions;
    definitions.push_back(f.definition());
    for (const Definition &def : f.updates()) {
        definitions.push_back(def);
    }

    for (size_t i = 0; i < definitions.size(); i++) {
        for (const Specialization &s : definitions[i].specializations()) {
            definitions.push_back(s.definition);
        }
    }

    for (const Definition &def : definitions) {
        const Schedule &s = def.schedule();
        for (const Dim &d : s.dims()) {
            if (!target.supports_device_api(d.device_api)) {
                user_error << "Schedule for Func " << f.name()
                           << " requires " << d.device_api
                           << " but no compatible target feature is enabled in target "
                           << target.to_string() << "\n";
            }
        }
    }

    LoopLevel store_at = f.schedule().store_level();
    LoopLevel compute_at = f.schedule().compute_level();

    // Outputs must be compute_root and store_root. They're really
    // store_in_user_code, but store_root is close enough.
    if (is_output) {
        if (store_at.is_root() && compute_at.is_root()) {
            return;
        } else {
            user_error << "Func " << f.name() << " is the output, so must"
                       << " be scheduled compute_root (which is the default).\n";
        }
    }

    // Inlining is allowed only if there is no specialization.
    if (store_at.is_inline() && compute_at.is_inline()) {
        user_assert(f.definition().specializations().empty())
            << "Func " << f.name() << " is scheduled inline, so it"
            << " must not have any specializations. Specialize on the"
            << " scheduled Func instead.\n";
        return;
    }

    // Otherwise inspect the uses to see what's ok.
    ComputeLegalSchedules legal(f, env);
    s.accept(&legal);

    bool store_at_ok = false, compute_at_ok = false;
    const vector<ComputeLegalSchedules::Site> &sites = legal.sites_allowed;
    size_t store_idx = 0, compute_idx = 0;
    for (size_t i = 0; i < sites.size(); i++) {
        if (sites[i].loop_level.match(store_at)) {
            store_at_ok = true;
            store_idx = i;
        }
        if (sites[i].loop_level.match(compute_at)) {
            compute_at_ok = store_at_ok;
            compute_idx = i;
        }
    }

    // Check there isn't a parallel loop between the compute_at and the store_at
    std::ostringstream err;

    if (store_at_ok && compute_at_ok) {
        for (size_t i = store_idx + 1; i <= compute_idx; i++) {
            if (sites[i].is_parallel) {
                err << "Func \"" << f.name()
                    << "\" is stored outside the parallel loop over "
                    << sites[i].loop_level.to_string()
                    << " but computed within it. This is a potential race condition.\n";
                store_at_ok = compute_at_ok = false;
            }
        }
    }

    if (!store_at_ok || !compute_at_ok) {
        err << "Func \"" << f.name() << "\" is computed at the following invalid location:\n"
            << "  " << schedule_to_source(f, store_at, compute_at) << "\n"
            << "Legal locations for this function are:\n";
        for (size_t i = 0; i < sites.size(); i++) {
            err << "  " << schedule_to_source(f, sites[i].loop_level, sites[i].loop_level) << "\n";
        }
        err << "\"" << f.name() << "\" is used in the following places:\n";
        PrintUsesOfFunc printer(f.name(), err);
        s.accept(&printer);

        user_error << err.str();
    }
}

void validate_fused_group_schedule_helper(const string &fn, size_t stage,
                                          const Definition &def_1,
                                          const map<string, Function> &env) {
    for (const auto &p : def_1.schedule().fused_pairs()) {
        internal_assert((fn == p.func_1) && (stage == p.stage_1));

        const auto iter1 = env.find(p.func_1);
        const auto iter2 = env.find(p.func_2);
        internal_assert(iter1 != env.end());

        if (iter2 == env.end()) { // The function is not used anywhere
            continue;
        }

        const Function &func_1 = iter1->second;
        const Function &func_2 = iter2->second;
        const Definition &def_2 = (p.stage_2 == 0) ? func_2.definition() : func_2.update(p.stage_2 - 1);

        // f2.compute_with(f1, var) is allowed only if f2 has no specializations.
        user_assert(func_2.definition().specializations().empty())
            << "Func " << func_2.name() << " is scheduled to be computed with "
            << func_1.name() << ", so it must not have any specializations.\n";

        // Verify that the functions being computed with are not scheduled inline.
        user_assert(!func_1.definition().schedule().compute_level().is_inline())
            << "Invalid compute_with: " << p.func_1 << ".s" << p.stage_1
            << " is scheduled inline.\n";
        user_assert(!func_2.definition().schedule().compute_level().is_inline())
            << "Invalid compute_with: " << p.func_2 << ".s" << p.stage_2
            << " is scheduled inline.\n";

        // Verify that the functions being computed with does not have extern definitions.
        user_assert(!func_1.has_extern_definition())
            << "Invalid compute_with: " << p.func_1 << ".s" << p.stage_1
            << " has extern definition.\n";
        user_assert(!func_2.has_extern_definition())
            << "Invalid compute_with: " << p.func_2 << ".s" << p.stage_2
            << " has extern definition.\n";

        // Verify that they are computed at the same loop level.
        user_assert((p.func_1 == p.func_2) ||
                    (func_1.definition().schedule().compute_level() ==
                     func_2.definition().schedule().compute_level()))
            << "Invalid compute_with: the compute levels of " << p.func_1 << ".s" << p.stage_1
            << " (computed at " << func_1.definition().schedule().compute_level().to_string()
            << ") and " << p.func_2 << ".s" << p.stage_2 << " ("
            << func_2.definition().schedule().compute_level().to_string() << ") do not match.\n";

        // Verify that their dimensions up to "var_name" are the same.
        const vector<Dim> &dims_1 = def_1.schedule().dims();
        const vector<Dim> &dims_2 = def_2.schedule().dims();

        // Assert that the variable specified in compute_with is in the dim list.
        const auto iter_1 = std::find_if(dims_1.begin(), dims_1.end(),
            [&p](const Dim& d) { return var_name_match(d.var, p.var_name); });
        user_assert(iter_1 != dims_1.end())
            << "Invalid compute_with: cannot find " << p.var_name << " in "
            << p.func_1 << ".s" << p.stage_1 << "\n";

        const auto iter_2 = std::find_if(dims_2.begin(), dims_2.end(),
            [&p](const Dim& d) { return var_name_match(d.var, p.var_name); });
        user_assert(iter_2 != dims_2.end())
            << "Invalid compute_with: cannot find " << p.var_name << " in "
            << p.func_2 << ".s" << p.stage_2 << "\n";

        size_t start_fuse_1 = iter_1 - dims_1.begin();
        size_t start_fuse_2 = iter_2 - dims_2.begin();

        int n_fused = dims_1.size() - start_fuse_1 - 1; // Ignore __outermost
        user_assert(n_fused == (int)(dims_2.size() - start_fuse_2 - 1))
            << "Invalid compute_with: # of fused dims of " << p.func_1 << ".s"
            << p.stage_1 << " and " << p.func_2 << ".s" << p.stage_2 << " do not match.\n";

        for (int i = 0; i < n_fused; ++i) {
            if (dims_1[start_fuse_1 + i] != dims_2[start_fuse_2 + i]) {
                user_error << "Invalid compute_with: dims " << i << " of " << p.func_1 << ".s"
                           << p.stage_1 << "(" << dims_1[start_fuse_1 + i].var << ") and " << p.func_2
                           << ".s" << p.stage_2 << "(" << dims_2[start_fuse_2 + i].var << ") do not match.\n";
            }
        }

        // If both stages computed_with are from the same Func, verify that the dims
        // computed with are the results of same application of splits/renames/etc.
        // Also, if it is a split dimension, verify that it doesn't use ShiftInwards
        // as tail strategy since this may affect correctness.
        if (p.func_1 == p.func_2) { // Update and its preceeding stage are fused
            const vector<string> pure_dims_1 = func_1.args();
            const vector<ReductionVariable> &rvars_1 = def_1.schedule().rvars();
            const vector<Split> &splits_1 = def_1.schedule().splits();
            const vector<Split> &splits_2 = def_2.schedule().splits();

            for (int i = 0; i < n_fused; ++i) {
                const string &var = dims_1[start_fuse_1 + i].var;
                {
                    const auto iter = std::find_if(pure_dims_1.begin(), pure_dims_1.end(),
                        [&var](const string& d) { return (d == var); });
                    if (iter != pure_dims_1.end()) {
                        // It is a pure var, no need to check the schedule.
                        continue;
                    }
                }
                {
                    const auto iter = std::find_if(rvars_1.begin(), rvars_1.end(),
                        [&var](const ReductionVariable& rv) { return (rv.var == var); });
                    if (iter != rvars_1.end()) {
                        // It is an rvar, no need to check the schedule.
                        continue;
                    }
                }
                // Relevant splits that produce this dim if there is any
                vector<Split> s_1, s_2;
                {
                    vector<string> relevant_dims = {var};
                    for (size_t j = splits_1.size(); j > 0; --j) {
                        const Split &s = splits_1[j-1];
                        bool relevant =
                            std::find_if(relevant_dims.begin(), relevant_dims.end(),
                                [&s](const string& d) { return (d == s.old_var); })
                            != relevant_dims.end();
                        relevant = relevant || (std::find_if(relevant_dims.begin(), relevant_dims.end(),
                                                    [&s](const string& d) { return (d == s.outer); })
                                                != relevant_dims.end());

                        if (s.is_split() || s.is_fuse()) {
                            relevant = relevant || (std::find_if(relevant_dims.begin(), relevant_dims.end(),
                                                        [&s](const string& d) { return (d == s.inner); })
                                                    != relevant_dims.end());
                        }
                        if (relevant) {
                            relevant_dims.push_back(s.old_var);
                            relevant_dims.push_back(s.outer);
                            if (s.is_split() || s.is_fuse()) {
                                relevant_dims.push_back(s.inner);
                            }
                            s_1.push_back(s);
                        }
                    }
                }
                {
                    vector<string> relevant_dims = {var};
                    for (size_t j = splits_2.size(); j > 0; --j) {
                        const Split &s = splits_2[j-1];
                        bool relevant =
                            std::find_if(relevant_dims.begin(), relevant_dims.end(),
                                [&s](const string& d) { return (d == s.old_var); })
                            != relevant_dims.end();
                        relevant = relevant || (std::find_if(relevant_dims.begin(), relevant_dims.end(),
                                                    [&s](const string& d) { return (d == s.outer); })
                                                != relevant_dims.end());

                        if (s.is_split() || s.is_fuse()) {
                            relevant = relevant || (std::find_if(relevant_dims.begin(), relevant_dims.end(),
                                                        [&s](const string& d) { return (d == s.inner); })
                                                    != relevant_dims.end());
                        }
                        if (relevant) {
                            relevant_dims.push_back(s.old_var);
                            relevant_dims.push_back(s.outer);
                            if (s.is_split() || s.is_fuse()) {
                                relevant_dims.push_back(s.inner);
                            }
                            s_2.push_back(s);
                        }
                    }
                }

                user_assert(s_1.size() == s_2.size())
                    << "Invalid compute_with: dim " << var << " in " << p.func_1 << ".s"
                    << p.stage_1 << " and " << p.func_2 << ".s" << p.stage_2
                    << " results from different schedules: " << s_1.size() << " vs. "
                    << s_2.size() << " schedules.\n";

                for (size_t k = 0; k < s_1.size(); ++k) {
                    const Split &s1 = s_1[k];
                    const Split &s2 = s_2[k];
                    bool match = (s1.split_type == s2.split_type) && (s1.old_var == s2.old_var) &&
                        (s1.outer == s2.outer) && equal(s1.factor, s2.factor) && (s1.exact == s2.exact);

                    if (s1.is_split() || s1.is_fuse()) {
                        match = match && (s1.inner == s2.inner);
                    }

                    user_assert(match)
                        << "Invalid compute_with: dim " << var << " in " << p.func_1 << ".s"
                        << p.stage_1 << ") and " << p.func_2 << ".s" << p.stage_2
                        << " results from different schedules.\n";

                    if (s1.is_split()) {
                        user_assert(s1.tail != TailStrategy::ShiftInwards)
                            << "When splitting Var " << s1.old_var
                            << " ShiftInwards is not a legal tail strategy since its inner/outer is fused, as"
                            << " it may change the meaning of the algorithm\n";
                    }
                }
            }
        }

    }
}

void validate_fused_groups_schedule(const vector<vector<string>> &fused_groups,
                                   const map<string, Function> &env) {
    for (const vector<string> &group : fused_groups) {
        for (const auto &fn : group) {
            const auto iter = env.find(fn);
            internal_assert(iter != env.end());

            validate_fused_group_schedule_helper(
                iter->first, 0, iter->second.definition(), env);
            for (size_t i = 0; i < iter->second.updates().size(); ++i) {
                validate_fused_group_schedule_helper(
                    iter->first, i + 1, iter->second.updates()[i], env);
            }
        }
    }
}

class RemoveLoopsOverOutermost : public IRMutator {
    using IRMutator::visit;

    void visit(const For *op) {
        if (ends_with(op->name, ".__outermost") &&
            is_one(simplify(op->extent)) &&
            op->device_api == DeviceAPI::None) {
            stmt = mutate(substitute(op->name, op->min, op->body));
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const LetStmt *op) {
        if (ends_with(op->name, ".__outermost.loop_extent") ||
            ends_with(op->name, ".__outermost.loop_min") ||
            ends_with(op->name, ".__outermost.loop_max")) {
            stmt = mutate(substitute(op->name, simplify(op->value), op->body));
        } else {
            IRMutator::visit(op);
        }
    }
};

Stmt schedule_functions(const vector<Function> &outputs,
                        const vector<string> &order,
                        const vector<vector<string>> &fused_groups,
                        const map<string, Function> &env,
                        const Target &target,
                        bool &any_memoized) {

    string root_var = LoopLevel::root().to_string();
    Stmt s = For::make(root_var, 0, 1, ForType::Serial, DeviceAPI::Host, Evaluate::make(0));

    any_memoized = false;

    validate_fused_groups_schedule(fused_groups, env);

    for (size_t i = fused_groups.size(); i > 0; --i) {
        const vector<string> &group = fused_groups[i-1];
        vector<Function> funcs(group.size());
        vector<bool> is_output_list(group.size(), false);
        for (size_t j = group.size(); j > 0; --j) {
            funcs[j-1] = env.find(group[j-1])->second;

            for (Function o : outputs) {
                is_output_list[j-1] = is_output_list[j-1] | o.same_as(funcs[j-1]);
            }
            validate_schedule(funcs[j-1], s, target, is_output_list[j-1], env);
            any_memoized = any_memoized || funcs[j-1].schedule().memoized();
        }

        internal_assert(!group.empty());

        int relevant_fused_pair = 0;
        for (const auto &pair : funcs[0].definition().schedule().fused_pairs()) {
            if (env.find(pair.func_2) == env.end()) {
                continue;
            }
            relevant_fused_pair += 1;
        }
        if ((group.size() == 1) && (relevant_fused_pair == 0)) {
            // There is only one function in the group and there is
            // no loop fusion among its definition
            if (funcs[0].can_be_inlined() &&
                funcs[0].schedule().compute_level().is_inline()) {
                debug(1) << "Inlining " << funcs[0].name() << '\n';
                s = inline_function(s, funcs[0]);
            } else {
                debug(1) << "Injecting realization of " << funcs[0].name() << '\n';
                InjectRealization injector(funcs[0], is_output_list[0], target, env);
                s = injector.mutate(s);
                internal_assert(injector.found_store_level && injector.found_compute_level);
            }
        } else {
            InjectGroupRealization injector(funcs, is_output_list, target, order, env);
            s = injector.mutate(s);
            internal_assert(injector.found_store_level && injector.found_compute_level);
        }

        debug(2) << s << '\n';
    }

    // We can remove the loop over root now
    const For *root_loop = s.as<For>();
    internal_assert(root_loop);
    s = root_loop->body;

    // We can also remove all the loops over __outermost now.
    s = RemoveLoopsOverOutermost().mutate(s);

    return s;

}

}
}

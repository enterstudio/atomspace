/**
 * Unify.cc
 *
 * Utilities for unifying atoms.
 *
 * Copyright (C) 2016 OpenCog Foundation
 * All Rights Reserved
 * Author: Nil Geisweiller
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Created by Linas Vepstas February 2008
 */

#include "Unify.h"

#include <opencog/util/algorithm.h>
#include <opencog/atoms/base/Atom.h>
#include <opencog/atoms/base/Node.h>
#include <opencog/atomutils/FindUtils.h>

namespace opencog {

UnificationSolutionSet::UnificationSolutionSet(bool s,
                                               const UnificationPartitions& p)
	: satisfiable(s), partitions(p)
{
}

TypedSubstitutions typed_substitutions(const UnificationSolutionSet& sol,
                                       const Handle& pre,
                                       const Handle& lhs, const Handle& rhs,
                                       Handle lhs_vardecl, Handle rhs_vardecl)
{
	OC_ASSERT(sol.satisfiable);

	TypedSubstitutions result;
	for (const UnificationPartition& partition : sol.partitions) {
		std::pair<HandleMap, Handle> ts;
		for (const UnificationPartition::value_type& typed_block : partition) {
			Handle least_abstract(Handle(createNode(VARIABLE_NODE,
			                                        "__dummy_top__")));
			for (const Handle& h : typed_block.first) {
				// Find the least abstract atom
				if (inherit(h, least_abstract) and
				    // If h is a variable, only consider it as value
				    // if it is in pre (stands for precedence)
				    (h->getType() != VARIABLE_NODE
				     or is_unquoted_unscoped_in_tree(pre, h)))
					least_abstract = h;
			}
			// Build variable mapping
			for (const Handle& var : typed_block.first) {
				if (var->getType() == VARIABLE_NODE)
					ts.first.insert({var, least_abstract});
			}
		}
		// Build the type for this variable. For now, the type is
		// merely lhs_vardecl and rhs_vardecl merged together. To do
		// well it should be taking into account the possibly more
		// restrictive types found during unification (i.e. the block
		// types).
		//
		// TODO: variables without declaration (i.e. rhs_vardecl or
		// lhs_vardecl are undefined) should be added here, borrowing
		// the variable declarations of equivalent variables if any.
		if (lhs.is_defined() and lhs_vardecl.is_undefined())
			lhs_vardecl = gen_vardecl(lhs);
		if (rhs.is_defined() and rhs_vardecl.is_undefined())
			rhs_vardecl = gen_vardecl(rhs);
		ts.second = merge_vardecl(rhs_vardecl, lhs_vardecl);

		result.insert(ts);
	}
	return result;
}

bool is_ill_quotation(BindLinkPtr bl)
{
	return bl->get_vardecl().is_undefined();
}

bool is_pm_connector(const Handle& h)
{
	return is_pm_connector(h->getType());
}

bool is_pm_connector(Type t)
{
	return t == AND_LINK or t == OR_LINK or t == NOT_LINK;
}

bool has_bl_variable_in_local_scope(BindLinkPtr bl, const Handle& scope)
{
	Handle var = scope->getOutgoingAtom(0)->getOutgoingAtom(0);
	return bl->get_variables().is_in_varset(var);
}

BindLinkPtr consume_ill_quotations(BindLinkPtr bl)
{
	Handle vardecl = bl->get_vardecl(),
		pattern = bl->get_body(),
		rewrite = bl->get_implicand();

	// Consume the pattern's quotations
	pattern = consume_ill_quotations(bl, pattern);

	// Consume the rewrite's quotations
	rewrite = consume_ill_quotations(bl, rewrite);

	// Recreate the BindLink
	return vardecl.is_defined() ?
		createBindLink(vardecl, pattern, rewrite)
		: createBindLink(pattern, rewrite);
}

Handle consume_ill_quotations(BindLinkPtr bl, Handle h,
                              Quotation quotation, bool escape)
{
	// Base case
	if (h->isNode())
		return h;

	// Recursive cases
	Type t = h->getType();
	if (quotation.consumable(t)) {
		if (t == QUOTE_LINK) {
			Handle scope = h->getOutgoingAtom(0);
			OC_ASSERT(classserver().isA(scope->getType(), SCOPE_LINK),
			          "This defaults the assumption, see this function comment");
			// Check whether a variable of the BindLink is present in
			// the local scope vardecl, if so escape the consumption.
			if (not has_bl_variable_in_local_scope(bl, scope)) {
				quotation.update(t);
				return consume_ill_quotations(bl, scope, quotation);
			} else {
				escape = true;
			}
		} else if (t == UNQUOTE_LINK) {
			if (not escape) {
				quotation.update(t);
				return consume_ill_quotations(bl, h->getOutgoingAtom(0),
				                              quotation);
			}
		}
		// Ignore LocalQuotes as they supposedly used only to quote
		// pattern matcher connectors.
	}

	quotation.update(t);
	HandleSeq consumed;
	for (const Handle outh : h->getOutgoingSet())
		consumed.push_back(consume_ill_quotations(bl, outh, quotation, escape));

	// TODO: call all factories
	bool is_scope = classserver().isA(t, SCOPE_LINK);
	return is_scope ? Handle(ScopeLink::factory(t, consumed))
		: Handle(createLink(t, consumed));
}

Handle substitute(BindLinkPtr bl, const TypedSubstitutions::value_type& ts)
{
	// Get the list of values to substitute from ts
	HandleSeq values = bl->get_variables().make_values(ts.first);

	// Perform alpha-conversion, this will work over valiues that are
	// non variables as well
	//
	// TODO: make sure that ts.second contains the declaration of all
	// variables
	Handle h = bl->alpha_conversion(values, ts.second);

	return Handle(consume_ill_quotations(BindLinkCast(h)));
}

UnificationSolutionSet unify(const Handle& lhs, const Handle& rhs,
                             const Handle& lhs_vardecl,
                             const Handle& rhs_vardecl,
                             Quotation lhs_quotation,
                             Quotation rhs_quotation)
{
	///////////////////
	// Base cases    //
	///////////////////

	// Make sure both handles are defined
	if (lhs == Handle::UNDEFINED or rhs == Handle::UNDEFINED)
		return UnificationSolutionSet(false);

	Type lhs_type(lhs->getType());
	Type rhs_type(rhs->getType());

	// If one is a node
	if (lhs->isNode() or rhs->isNode()) {
		// If one is an unquoted variable, then unifies, otherwise
		// check their equality
		if ((lhs_quotation.is_unquoted() and lhs_type == VARIABLE_NODE)
		    or (rhs_quotation.is_unquoted() and rhs_type == VARIABLE_NODE)) {
			return mkvarsol(lhs, rhs, lhs_vardecl, rhs_vardecl,
			                lhs_quotation, rhs_quotation);
		} else
			return UnificationSolutionSet(lhs == rhs);
	}

	////////////////////////
	// Recursive cases    //
	////////////////////////

	// Consume quotations
	if (lhs_quotation.consumable(lhs_type)
	    and rhs_quotation.consumable(rhs_type)) {
		lhs_quotation.update(lhs_type);
		rhs_quotation.update(rhs_type);
		return unify(lhs->getOutgoingAtom(0), rhs->getOutgoingAtom(0),
		             lhs_vardecl, rhs_vardecl,
		             lhs_quotation, rhs_quotation);
	}
	if (lhs_quotation.consumable(lhs_type)) {
		lhs_quotation.update(lhs_type);
		return unify(lhs->getOutgoingAtom(0), rhs, lhs_vardecl, rhs_vardecl,
		             lhs_quotation, rhs_quotation);
	}
	if (rhs_quotation.consumable(rhs_type)) {
		rhs_quotation.update(rhs_type);
		return unify(lhs, rhs->getOutgoingAtom(0), lhs_vardecl, rhs_vardecl,
		             lhs_quotation, rhs_quotation);
	}

	// Update quotations
	lhs_quotation.update(lhs_type);
	rhs_quotation.update(rhs_type);

	// At least one of them is a link, check if they have the same
	// type (e.i. do they match so far)
	if (lhs_type != rhs_type)
		return UnificationSolutionSet(false);

	// At this point they are both links of the same type, check that
	// they have the same arity
	Arity lhs_arity(lhs->getArity());
	Arity rhs_arity(rhs->getArity());
	if (lhs_arity != rhs_arity)
		return UnificationSolutionSet(false);

	if (is_unordered(rhs))
		return unordered_unify(lhs->getOutgoingSet(), rhs->getOutgoingSet(),
		                       lhs_vardecl, rhs_vardecl,
		                       lhs_quotation, rhs_quotation);
	else
		return ordered_unify(lhs->getOutgoingSet(), rhs->getOutgoingSet(),
		                     lhs_vardecl, rhs_vardecl,
		                     lhs_quotation, rhs_quotation);
}

UnificationSolutionSet unordered_unify(const HandleSeq& lhs,
                                       const HandleSeq& rhs,
                                       const Handle& lhs_vardecl,
                                       const Handle& rhs_vardecl,
                                       Quotation lhs_quotation,
                                       Quotation rhs_quotation)
{
	Arity lhs_arity(lhs.size());
	Arity rhs_arity(rhs.size());
	OC_ASSERT(lhs_arity == rhs_arity);

	// Base case
	if (lhs_arity == 0)
		return UnificationSolutionSet();

	// Recursive case
	UnificationSolutionSet sol(false);
	for (Arity i = 0; i < lhs_arity; ++i) {
		auto head_sol = unify(lhs[i], rhs[0], lhs_vardecl, rhs_vardecl,
		                      lhs_quotation, rhs_quotation);
		if (head_sol.satisfiable) {
			HandleSeq lhs_tail(cp_erase(lhs, i));
			HandleSeq rhs_tail(cp_erase(rhs, 0));
			auto tail_sol = unordered_unify(lhs_tail, rhs_tail,
			                                lhs_vardecl, rhs_vardecl,
			                                lhs_quotation, rhs_quotation);
			UnificationSolutionSet perm_sol = join(head_sol, tail_sol);
			// Union merge satisfiable permutations
			if (perm_sol.satisfiable) {
				sol.satisfiable = true;
				sol.partitions.insert(perm_sol.partitions.begin(),
				                      perm_sol.partitions.end());
			}
		}
	}
	return sol;
}

UnificationSolutionSet ordered_unify(const HandleSeq& lhs,
                                     const HandleSeq& rhs,
                                     const Handle& lhs_vardecl,
                                     const Handle& rhs_vardecl,
                                     Quotation lhs_quotation,
                                     Quotation rhs_quotation)
{
	Arity lhs_arity(lhs.size());
	Arity rhs_arity(rhs.size());
	OC_ASSERT(lhs_arity == rhs_arity);

	UnificationSolutionSet sol;
	for (Arity i = 0; i < lhs_arity; ++i) {
		auto rs = unify(lhs[i], rhs[i], lhs_vardecl, rhs_vardecl,
		                lhs_quotation, rhs_quotation);
		sol = join(sol, rs);
		if (not sol.satisfiable)     // Stop if unification has failed
			break;
	}
	return sol;
}
	
bool is_unordered(const Handle& h)
{
	return classserver().isA(h->getType(), UNORDERED_LINK);
}

HandleSeq cp_erase(const HandleSeq& hs, Arity i)
{
	HandleSeq hs_cp(hs);
	hs_cp.erase(hs_cp.begin() + i);
	return hs_cp;
}

UnificationSolutionSet mkvarsol(const Handle& lhs, const Handle& rhs,
                                const Handle& lhs_vardecl,
                                const Handle& rhs_vardecl,
                                Quotation lhs_quotation,
                                Quotation rhs_quotation)
{
	Handle inter = type_intersection(lhs, rhs, lhs_vardecl, rhs_vardecl,
	                                 lhs_quotation, rhs_quotation);
	if (inter == Handle::UNDEFINED)
		return UnificationSolutionSet(false);
	else {
		OrderedHandleSet hset{lhs, rhs};
		UnificationPartitions par{{{hset, inter}}};
		return UnificationSolutionSet(true, par);
	}
}

UnificationSolutionSet join(const UnificationSolutionSet& lhs,
                            const UnificationSolutionSet& rhs)
{
	// No need to join if one of them is non satisfiable
	if (not lhs.satisfiable or not rhs.satisfiable)
		return UnificationSolutionSet(false);

	// No need to join if one of them is empty
	if (rhs.partitions.empty())
		return lhs;
	if (lhs.partitions.empty())
		return rhs;

	// By now both are satisfiable and non empty, join them
	UnificationSolutionSet result;
	for (const UnificationPartition& rp : rhs.partitions) {
		UnificationPartitions sol(join(lhs.partitions, rp));
		result.partitions.insert(sol.begin(), sol.end());
	}

	// If we get an empty join while the inputs where not empty then
	// the join has failed
	result.satisfiable = not result.partitions.empty();

	return result;
}

UnificationPartitions join(const UnificationPartitions& lhs,
                           const UnificationPartition& rhs)
{
	// Base cases
	if (rhs.empty())
		return lhs;
	if (lhs.empty())
		return {rhs};

	// Recursive case (a loop actually)
	UnificationPartitions result;
	for (const auto& par : lhs) {
		UnificationPartition jo = join(par, rhs);
		if (not jo.empty())
			result.insert(jo);
	}
	return result;
}

UnificationPartition join(const UnificationPartition& lhs,
                          const UnificationPartition& rhs)
{
	// Don't bother joining if one of them is empty
	if (lhs.empty())
		return rhs;
	if (rhs.empty())
		return lhs;

	// Join
	UnificationPartition result(lhs);
	for (const auto& typed_block : rhs) {
		for (auto it = lhs.begin(); it != lhs.end(); ++it) {
			if (has_empty_intersection(typed_block.first, it->first)) {
				// Merely insert this independent block
				result.insert(typed_block);
			} else {
				// Join the 2 equality related blocks
				UnificationBlock m_typed_block = join(typed_block, *it);
				// If the resulting block is satisfiable then replace *it
				if (is_satisfiable(m_typed_block)) {
					result.erase(it->first);
					result.insert(m_typed_block);
				}
				// If the resulting block is non satisfiable then the
				// partition is non satisfiable as well, thus an empty
				// partition is returned
				else {
					return UnificationPartition();
				}
			}
		}
	}
	return result;
}

UnificationBlock join(const UnificationBlock& lhs, const UnificationBlock& rhs)
{
	return {set_union(lhs.first, rhs.first),
			type_intersection(lhs.second, rhs.second)};
}

bool is_satisfiable(const UnificationBlock& block)
{
	return block.second != Handle::UNDEFINED;
}

// TODO: very limited type intersection, should support structural
// types, etc.
Handle type_intersection(const Handle& lhs, const Handle& rhs,
                         const Handle& lhs_vardecl, const Handle& rhs_vardecl,
                         Quotation lhs_quotation, Quotation rhs_quotation)
{
	if (inherit(lhs, rhs, lhs_vardecl, rhs_vardecl,
	            lhs_quotation, rhs_quotation))
		return lhs;
	if (inherit(rhs, lhs, rhs_vardecl, lhs_vardecl,
	            rhs_quotation, lhs_quotation))
		return rhs;
	return Handle::UNDEFINED;
}

std::set<Type> simplify_type_union(std::set<Type>& type)
{
	return {}; // TODO: do we really need that?
}

std::set<Type> get_union_type(const Handle& h, const Handle& vardecl)
{
	VariableListPtr vardecl_vlp(gen_varlist(h, vardecl));
	const Variables& variables(vardecl_vlp->get_variables());
	const VariableTypeMap& vtm = variables._simple_typemap;
	auto it = vtm.find(h);
	if (it == vtm.end() or it->second.empty())
		return {ATOM};
	else {
		return it->second;
	}
}

bool inherit(const Handle& lhs, const Handle& rhs,
             const Handle& lhs_vardecl, const Handle& rhs_vardecl,
             Quotation lhs_quotation, Quotation rhs_quotation)
{
	Type lhs_type = lhs->getType();
	Type rhs_type = rhs->getType();

	// Recursive cases

	// Consume quotations
	if (lhs_quotation.consumable(lhs_type)) {
		lhs_quotation.update(lhs_type);
		return inherit(lhs->getOutgoingAtom(0), rhs, lhs_vardecl, rhs_vardecl,
		               lhs_quotation, rhs_quotation);
	}
	if (rhs_quotation.consumable(rhs_type)) {
		rhs_quotation.update(rhs_type);
		return inherit(lhs, rhs->getOutgoingAtom(0), lhs_vardecl, rhs_vardecl,
		               lhs_quotation, rhs_quotation);
	}

	// Base cases

	if (lhs == rhs)
		return true;

	if (lhs_quotation.is_unquoted() and VARIABLE_NODE == lhs_type
	    and rhs_quotation.is_unquoted() and VARIABLE_NODE == rhs_type)
		return inherit(get_union_type(lhs, lhs_vardecl),
		               get_union_type(rhs, rhs_vardecl));

	if (rhs_quotation.is_unquoted())
		return gen_varlist(rhs, rhs_vardecl)->is_type(rhs, lhs);

	return false;
}

bool inherit(const Handle& lhs, const Handle& rhs)
{
	return VARIABLE_NODE == rhs->getType() or lhs == rhs;
}

bool inherit(Type lhs, Type rhs)
{
	return classserver().isA(lhs, rhs);
}

bool inherit(Type lhs, const std::set<Type>& rhs)
{
	for (Type ty : rhs)
		if (inherit(lhs, ty))
			return true;
	return false;
}

bool inherit(const std::set<Type>& lhs, const std::set<Type>& rhs)
{
	for (Type ty : lhs)
		if (not inherit(ty, rhs))
			return false;
	return true;
}

/**
 * Generate a VariableList of the free variables of a given atom h.
 */
VariableListPtr gen_varlist(const Handle& h)
{
	OrderedHandleSet vars = get_free_variables(h);
	return createVariableList(HandleSeq(vars.begin(), vars.end()));
}

Handle gen_vardecl(const Handle& h)
{
	return Handle(gen_varlist(h));
}

/**
 * Given an atom h and its variable declaration vardecl, turn the
 * vardecl into a VariableList if not already, and if undefined,
 * generate a VariableList of the free variables of h.
 */
VariableListPtr gen_varlist(const Handle& h, const Handle& vardecl)
{
	if (vardecl == Handle::UNDEFINED)
		return gen_varlist(h);
	else {
		Type vardecl_t = vardecl->getType();
		if (vardecl_t == VARIABLE_LIST)
			return VariableListCast(vardecl);
		else {
			OC_ASSERT(vardecl_t == VARIABLE_NODE
			          or vardecl_t == TYPED_VARIABLE_LINK);
			return createVariableList(vardecl);
		}
	}
}

Handle merge_vardecl(const Handle& lhs_vardecl, const Handle& rhs_vardecl)
{
	if (lhs_vardecl.is_undefined())
		return rhs_vardecl;
	if (rhs_vardecl.is_undefined())
		return lhs_vardecl;

	VariableList
		lhs_vl(lhs_vardecl),
		rhs_vl(rhs_vardecl);

	const Variables& lhs_vars = lhs_vl.get_variables();
	Variables new_vars = rhs_vl.get_variables();

	new_vars.extend(lhs_vars);

	return new_vars.get_vardecl();
}

std::string oc_to_string(const UnificationBlock& ub)
{
	std::stringstream ss;
	ss << "block:" << std::endl << oc_to_string(ub.first)
	   << "type:" << std::endl << oc_to_string(ub.second);
	return ss.str();
}

std::string oc_to_string(const UnificationPartition& up)
{
	std::stringstream ss;
	ss << "size = " << up.size() << std::endl;
	int i = 0;
	for (const auto& p : up) {
		ss << "block[" << i << "]:" << std::endl << oc_to_string(p.first)
		   << "type[" << i << "]:" << std::endl << oc_to_string(p.second);
		i++;
	}
	return ss.str();
}

std::string oc_to_string(const UnificationPartitions& par)
{
	std::stringstream ss;
	ss << "size = " << par.size() << std::endl;
	int i = 0;
	for (const auto& el : par) {
		ss << "typed partition[" << i << "]:" << std::endl << oc_to_string(el);
		i++;
	}
	return ss.str();
}

std::string oc_to_string(const UnificationSolutionSet& sol)
{
	std::stringstream ss;
	ss << "satisfiable: " << sol.satisfiable << std::endl
	   << "partitions: " << oc_to_string(sol.partitions);
	return ss.str();
}

std::string oc_to_string(const TypedSubstitutions& tss)
{
	std::stringstream ss;
	ss << "size = " << tss.size() << std::endl;
	int i = 0;
	for (const auto& ts : tss)
		ss << "typed substitution[" << i << "]:" << std::endl
		   << oc_to_string(ts);
	return ss.str();
}

std::string oc_to_string(const TypedSubstitution& ts)
{
	std::stringstream ss;
	ss << "substitution:" << std::endl << oc_to_string(ts.first)
	   << "type:" << std::endl << oc_to_string(ts.second);
	return ss.str();
}

} // namespace opencog

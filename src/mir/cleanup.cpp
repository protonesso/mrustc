/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/cleanup.cpp
 * - MIR Cleanup
 *
 * Removes artefacts left after monomorphisation
 * - Converts <Trait as Trait>::method() into a vtable call
 * - Replaces constants by their value
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>

struct MirMutator
{
    ::MIR::Function& m_fcn;
    unsigned int    cur_block;
    unsigned int    cur_stmt;
    mutable ::std::vector< ::MIR::Statement>    new_statements;
    
    MirMutator(::MIR::Function& fcn, unsigned int bb, unsigned int stmt):
        m_fcn(fcn),
        cur_block(bb), cur_stmt(stmt)
    {
    }
    
    ::MIR::LValue new_temporary(::HIR::TypeRef ty)
    {
        auto rv = ::MIR::LValue::make_Temporary({ static_cast<unsigned int>(m_fcn.temporaries.size()) });
        m_fcn.temporaries.push_back( mv$(ty) );
        return rv;
    }
    
    void push_statement(::MIR::Statement stmt)
    {
        new_statements.push_back( mv$(stmt) );
    }
    
    ::MIR::LValue in_temporary(::HIR::TypeRef ty, ::MIR::RValue val)
    {
        auto rv = this->new_temporary( mv$(ty) );
        push_statement( ::MIR::Statement::make_Assign({ rv.clone(), mv$(val) }) );
        return rv;
    }
    
    decltype(new_statements.begin()) flush()
    {
        DEBUG("flush - " << cur_block << "/" << cur_stmt);
        auto& block = m_fcn.blocks.at(cur_block);
        assert( cur_stmt <= block.statements.size() );
        auto it = block.statements.begin() + cur_stmt;
        for(auto& stmt : new_statements)
        {
            DEBUG("- Push stmt @" << cur_stmt << " (size=" << block.statements.size() + 1 << ")");
            it = block.statements.insert(it, mv$(stmt));
            ++ it;
            cur_stmt += 1;
        }
        new_statements.clear();
        return it;
    }
};

const ::HIR::Literal* MIR_Cleanup_GetConstant(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Path& path,  ::HIR::TypeRef& out_ty)
{
    TU_MATCHA( (path.m_data), (pe),
    (Generic,
        const auto& constant = resolve.m_crate.get_constant_by_path(sp, pe.m_path);
        if( pe.m_params.m_types.size() != 0 )
            TODO(sp, "Generic constants - " << path);
        out_ty = constant.m_type.clone();
        return &constant.m_value_res;
        ),
    (UfcsUnknown,
        ),
    (UfcsKnown,
        ),
    (UfcsInherent,
        )
    )
    return nullptr;
}

::MIR::LValue MIR_Cleanup_Virtualize(
    const Span& sp, const ::MIR::TypeResolve& state, MirMutator& mutator,
    ::MIR::LValue& receiver_lvp,
    const ::HIR::TypeRef::Data::Data_TraitObject& te, const ::HIR::Path::Data::Data_UfcsKnown& pe
    )
{
    assert( te.m_trait.m_trait_ptr );
    const auto& trait = *te.m_trait.m_trait_ptr;
    
    // 1. Get the vtable index for this function
    auto it = trait.m_value_indexes.find( pe.item );
    while( it != trait.m_value_indexes.end() )
    {
        DEBUG("- " << it->second.second);
        if( it->second.second.m_path == pe.trait.m_path )
        {
            // TODO: Match generics using match_test_generics comparing to the trait args
            break ;
        }
        ++ it;
    }
    if( it == trait.m_value_indexes.end() || it->first != pe.item )
        BUG(sp, "Calling method '" << pe.item << "' from " << pe.trait << " through " << te.m_trait.m_path << " which isn't in the vtable");
    unsigned int vtable_idx = it->second.first;
    
    // 2. Load from the vtable
    auto vtable_ty_spath = te.m_trait.m_path.m_path;
    vtable_ty_spath.m_components.back() += "#vtable";
    const auto& vtable_ref = state.m_resolve.m_crate.get_struct_by_path(sp, vtable_ty_spath);
    // Copy the param set from the trait in the trait object
    ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
    // - Include associated types on bound
    for(const auto& ty_b : te.m_trait.m_type_bounds) {
        auto idx = trait.m_type_indexes.at(ty_b.first);
        if(vtable_params.m_types.size() <= idx)
            vtable_params.m_types.resize(idx+1);
        vtable_params.m_types[idx] = ty_b.second.clone();
    }
    auto vtable_ty = ::HIR::TypeRef::new_pointer(
        ::HIR::BorrowType::Shared,
        ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref )
        );
    
    // Allocate a temporary for the vtable pointer itself
    auto vtable_lv = mutator.new_temporary( mv$(vtable_ty) );
    // - Load the vtable and store it
    auto vtable_rval = ::MIR::RValue::make_DstMeta({ ::MIR::LValue::make_Deref({ box$(receiver_lvp.clone()) }) });
    mutator.push_statement( ::MIR::Statement::make_Assign({ vtable_lv.clone(), mv$(vtable_rval) }) );
    
    auto ptr_rval = ::MIR::RValue::make_DstPtr({ ::MIR::LValue::make_Deref({ box$(receiver_lvp.clone()) }) });
    auto ptr_lv = mutator.new_temporary( ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit()) );
    mutator.push_statement( ::MIR::Statement::make_Assign({ ptr_lv.clone(), mv$(ptr_rval) }) );
    receiver_lvp = mv$(ptr_lv);
    
    // Update the terminator with the new information.
    return ::MIR::LValue::make_Field({ box$(::MIR::LValue::make_Deref({ box$(vtable_lv) })), vtable_idx });
}

::MIR::RValue MIR_Cleanup_Unsize(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty_inner, ::MIR::LValue ptr_value)
{
    const auto& dst_ty_inner = (dst_ty.m_data.is_Borrow() ? *dst_ty.m_data.as_Borrow().inner : *dst_ty.m_data.as_Pointer().inner);
    
    TU_MATCH_DEF( ::HIR::TypeRef::Data, (dst_ty_inner.m_data), (die),
    (
        // Dunno?
        MIR_TODO(state, "Unsize to pointer to " << dst_ty_inner);
        ),
    (Generic,
        // Emit a cast rvalue
        return ::MIR::RValue::make_Cast({ mv$(ptr_value), dst_ty.clone() });
        ),
    (Slice,
        if( src_ty_inner.m_data.is_Array() )
        {
            const auto& in_array = src_ty_inner.m_data.as_Array();
            auto size_lval = mutator.in_temporary( ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::Constant( static_cast<uint64_t>(in_array.size_val) ) );
            return ::MIR::RValue::make_MakeDst({ mv$(ptr_value), mv$(size_lval) });
        }
        else if( src_ty_inner.m_data.is_Generic() || (src_ty_inner.m_data.is_Path() && src_ty_inner.m_data.as_Path().binding.is_Opaque()) )
        {
            // HACK: FixedSizeArray uses `A: Unsize<[T]>` which will lead to the above code not working (as the size isn't known).
            // - Maybe _Meta on the `&A` would work as a stopgap (since A: Sized, it won't collide with &[T] or similar)
            auto size_lval = mutator.in_temporary( ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::RValue::make_DstMeta({ ptr_value.clone() }) );
            return ::MIR::RValue::make_MakeDst({ mv$(ptr_value), mv$(size_lval) });
        }
        else
        {
            MIR_BUG(state, "Unsize to slice from non-array - " << src_ty_inner);
        }
        ),
    (TraitObject,
        auto unit_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit());
        // If the data trait hasn't changed, re-create the DST
        if( src_ty_inner.m_data.is_TraitObject() )
        {
            auto inner_ptr_lval = mutator.in_temporary( unit_ptr.clone(), ::MIR::RValue::make_DstPtr({ ptr_value.clone() }) );
            auto vtable_lval = mutator.in_temporary( unit_ptr.clone(), ::MIR::RValue::make_DstMeta({ ptr_value.clone() }) );
            return ::MIR::RValue::make_MakeDst({ mv$(inner_ptr_lval), mv$(vtable_lval) });
        }
        else
        {
            // Obtain the vtable if the destination is a trait object vtable exists as an unnamable associated type
            ::MIR::LValue   vtable_lval;
            if( die.m_trait.m_path.m_path == ::HIR::SimplePath() )
            {
                auto null_lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::Constant::make_Uint(0u) );
                auto unit_ptr_2 = unit_ptr.clone();
                vtable_lval = mutator.in_temporary( mv$(unit_ptr), ::MIR::RValue::make_Cast({ mv$(null_lval), mv$(unit_ptr_2) }) );
            }
            else
            {
                const auto& trait = *die.m_trait.m_trait_ptr;

                auto vtable_ty_spath = die.m_trait.m_path.m_path;
                vtable_ty_spath.m_components.back() += "#vtable";
                const auto& vtable_ref = state.m_crate.get_struct_by_path(state.sp, vtable_ty_spath);
                // Copy the param set from the trait in the trait object
                ::HIR::PathParams   vtable_params = die.m_trait.m_path.m_params.clone();
                // - Include associated types on bound
                for(const auto& ty_b : die.m_trait.m_type_bounds) {
                    auto idx = trait.m_type_indexes.at(ty_b.first);
                    if(vtable_params.m_types.size() <= idx)
                        vtable_params.m_types.resize(idx+1);
                    vtable_params.m_types[idx] = ty_b.second.clone();
                }
                auto vtable_type = ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref );
                    
                ::HIR::Path vtable { src_ty_inner.clone(), die.m_trait.m_path.clone(), "#vtable" };
                vtable_lval = mutator.in_temporary(
                    ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, mv$(vtable_type)),
                    ::MIR::RValue( ::MIR::Constant::make_ItemAddr(mv$(vtable)) )
                    );
                    
            }
            return ::MIR::RValue::make_MakeDst({ mv$(ptr_value), mv$(vtable_lval) });
        }
        )
    )
}

::MIR::RValue MIR_Cleanup_CoerceUnsized(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty, ::MIR::LValue value)
{
    //  > Path -> Path = Unsize
    // (path being destination is otherwise invalid)
    if( dst_ty.m_data.is_Path() )
    {
        MIR_ASSERT(state, src_ty.m_data.is_Path(), "CoerceUnsized to Path must have a Path source - " << src_ty << " to " << dst_ty);
        const auto& dte = dst_ty.m_data.as_Path();
        const auto& ste = src_ty.m_data.as_Path();
        
        // - Types must differ only by a single field, and be from the same definition
        MIR_ASSERT(state, dte.binding.is_Struct(), "Note, can't CoerceUnsized non-structs");
        MIR_ASSERT(state, dte.binding.tag() == ste.binding.tag(),
            "Note, can't CoerceUnsized mismatched structs - " << src_ty << " to " << dst_ty);
        MIR_ASSERT(state, dte.binding.as_Struct() == ste.binding.as_Struct(),
            "Note, can't CoerceUnsized mismatched structs - " << src_ty << " to " << dst_ty);
        const auto& str = *dte.binding.as_Struct();
        MIR_ASSERT(state, str.m_markings.coerce_unsized_index != ~0u,
            "Struct " << src_ty << " doesn't impl CoerceUnsized");
        
        auto monomorph_cb_d = monomorphise_type_get_cb(state.sp, nullptr, &dte.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph_cb_s = monomorphise_type_get_cb(state.sp, nullptr, &ste.path.m_data.as_Generic().m_params, nullptr);
        
        // - Destructure and restrucure with the unsized fields
        ::std::vector<::MIR::LValue>    ents;
        TU_MATCHA( (str.m_data), (se),
        (Unit,
            MIR_BUG(state, "Unit-like struct CoerceUnsized is impossible - " << src_ty);
            ),
        (Tuple,
            ents.reserve( se.size() );
            for(unsigned int i = 0; i < se.size(); i++)
            {
                if( i == str.m_markings.coerce_unsized_index )
                {
                    auto ty_d = monomorphise_type_with(state.sp, se[i].ent, monomorph_cb_d, false);
                    auto ty_s = monomorphise_type_with(state.sp, se[i].ent, monomorph_cb_s, false);
                    
                    auto new_rval = MIR_Cleanup_CoerceUnsized(state, mutator, ty_d, ty_s,  ::MIR::LValue::make_Field({ box$(value.clone()), i }));
                    auto new_lval = mutator.new_temporary( mv$(ty_d) );
                    mutator.push_statement( ::MIR::Statement::make_Assign({ new_lval.clone(), mv$(new_rval) }) );
                    
                    ents.push_back( mv$(new_lval) );
                }
                else
                {
                    ents.push_back( ::MIR::LValue::make_Field({ box$(value.clone()), i}) );
                }
            }
            ),
        (Named,
            ents.reserve( se.size() );
            for(unsigned int i = 0; i < se.size(); i++)
            {
                if( i == str.m_markings.coerce_unsized_index ) {
                    auto ty_d = monomorphise_type_with(state.sp, se[i].second.ent, monomorph_cb_d, false);
                    auto ty_s = monomorphise_type_with(state.sp, se[i].second.ent, monomorph_cb_s, false);
                    
                    auto new_rval = MIR_Cleanup_CoerceUnsized(state, mutator, ty_d, ty_s,  ::MIR::LValue::make_Field({ box$(value.clone()), i }));
                    auto new_lval = mutator.new_temporary( mv$(ty_d) );
                    mutator.push_statement( ::MIR::Statement::make_Assign({ new_lval.clone(), mv$(new_rval) }) );
                    
                    ents.push_back( mv$(new_lval) );
                }
                else {
                    ents.push_back( ::MIR::LValue::make_Field({ box$(value.clone()), i}) );
                }
            }
            )
        )
        return ::MIR::RValue::make_Struct({ dte.path.m_data.as_Generic().clone(), ~0u, mv$(ents) });
    }
    
    if( dst_ty.m_data.is_Borrow() )
    {
        MIR_ASSERT(state, src_ty.m_data.is_Borrow(), "CoerceUnsized to Borrow must have a Borrow source - " << src_ty << " to " << dst_ty);
        const auto& ste = src_ty.m_data.as_Borrow();
        
        return MIR_Cleanup_Unsize(state, mutator, dst_ty, *ste.inner, mv$(value));
    }
    
    // Pointer Coercion - Downcast and unsize
    if( dst_ty.m_data.is_Pointer() )
    {
        MIR_ASSERT(state, src_ty.m_data.is_Pointer(), "CoerceUnsized to Pointer must have a Pointer source - " << src_ty << " to " << dst_ty);
        const auto& dte = dst_ty.m_data.as_Pointer();
        const auto& ste = src_ty.m_data.as_Pointer();
        
        if( dte.type == ste.type )
        {
            // TODO: Use unsize code above
            return MIR_Cleanup_Unsize(state, mutator, dst_ty, *ste.inner, mv$(value));
        }
        else
        {
            MIR_ASSERT(state, *dte.inner == *ste.inner, "TODO: Can pointer CoerceUnsized unsize? " << src_ty << " to " << dst_ty);
            MIR_ASSERT(state, dte.type < ste.type, "CoerceUnsize attempting to raise pointer type");
            
            return ::MIR::RValue::make_Cast({ mv$(value), dst_ty.clone() });
        }
    }
    
    MIR_BUG(state, "Unknown CoerceUnsized target " << dst_ty << " from " << src_ty);
    throw "";
}

void MIR_Cleanup(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    
    MirMutator  mutator { fcn, 0, 0 };
    for(auto& block : fcn.blocks)
    {
        for(auto it = block.statements.begin(); it != block.statements.end(); ++ it)
        {
            state.set_cur_stmt( mutator.cur_block, mutator.cur_stmt );
            auto& stmt = *it;
            
            if( stmt.is_Assign() )
            {
                auto& se = stmt.as_Assign();
                
                TU_IFLET( ::MIR::RValue, se.src, Constant, e,
                    // TODO: Replace `Const` with actual values
                    TU_IFLET( ::MIR::Constant, e, Const, ce,
                        // 1. Find the constant
                        ::HIR::TypeRef  ty;
                        const auto* lit_ptr = MIR_Cleanup_GetConstant(sp, resolve, ce.p, ty);
                        if( lit_ptr )
                        {
                            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
                            (
                                //TODO(sp, "Literal of type " << ty << " - " << *lit_ptr);
                                ),
                            (Primitive,
                                switch(te)
                                {
                                case ::HIR::CoreType::Char:
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::U64:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U8:
                                    e = ::MIR::Constant::make_Uint( lit_ptr->as_Integer() );
                                    break;
                                case ::HIR::CoreType::Isize:
                                case ::HIR::CoreType::I64:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I8:
                                    e = ::MIR::Constant::make_Int( lit_ptr->as_Integer() );
                                    break;
                                case ::HIR::CoreType::F64:
                                case ::HIR::CoreType::F32:
                                    e = ::MIR::Constant::make_Float( lit_ptr->as_Float() );
                                    break;
                                case ::HIR::CoreType::Bool:
                                    e = ::MIR::Constant::make_Bool( !!lit_ptr->as_Integer() );
                                    break;
                                case ::HIR::CoreType::Str:
                                    BUG(sp, "Const of type `str` - " << ce.p);
                                }
                                ),
                            (Pointer,
                                if( lit_ptr->is_BorrowOf() ) {
                                    // TODO: 
                                }
                                else {
                                    auto lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::RValue( ::MIR::Constant::make_Uint( lit_ptr->as_Integer() ) ) );
                                    se.src = ::MIR::RValue::make_Cast({ mv$(lval), mv$(ty) });
                                }
                                ),
                            (Borrow,
                                if( lit_ptr->is_BorrowOf() ) {
                                    // TODO: 
                                }
                                else if( te.inner->m_data.is_Slice() && *te.inner->m_data.as_Slice().inner == ::HIR::CoreType::U8 ) {
                                    ::std::vector<uint8_t>  bytestr;
                                    for(auto v : lit_ptr->as_String())
                                        bytestr.push_back( static_cast<uint8_t>(v) );
                                    e = ::MIR::Constant::make_Bytes( mv$(bytestr) );
                                }
                                else if( te.inner->m_data.is_Array() && *te.inner->m_data.as_Array().inner == ::HIR::CoreType::U8 ) {
                                    // TODO: How does this differ at codegen to the above?
                                    ::std::vector<uint8_t>  bytestr;
                                    for(auto v : lit_ptr->as_String())
                                        bytestr.push_back( static_cast<uint8_t>(v) );
                                    e = ::MIR::Constant::make_Bytes( mv$(bytestr) );
                                }
                                else if( *te.inner == ::HIR::CoreType::Str ) {
                                    e = ::MIR::Constant::make_StaticString( lit_ptr->as_String() );
                                }
                                else {
                                    TODO(sp, "Const with type " << ty);
                                }
                                )
                            )
                        }
                    )
                )
                
                if( se.src.is_Cast() )
                {
                    auto& e = se.src.as_Cast();
                    ::HIR::TypeRef  tmp;
                    const auto& src_ty = state.get_lvalue_type(tmp, e.val);
                    // TODO: Unsize and CoerceUnsized operations
                    // - Unsize should create a fat pointer if the pointer class is known (vtable or len)
                    TU_IFLET( ::HIR::TypeRef::Data, e.type.m_data, Borrow, te
                        //  > & -> & = Unsize, create DST based on the pointer class of the destination.
                        // (&-ptr being destination is otherwise invalid)
                        // TODO Share with the CoerceUnsized handling?
                    )
                    // - CoerceUnsized should re-create the inner type if known.
                    else TU_IFLET( ::HIR::TypeRef::Data, e.type.m_data, Path, te,
                        TU_IFLET( ::HIR::TypeRef::Data, src_ty.m_data, Path, ste,
                            ASSERT_BUG( sp, ! te.binding.is_Unbound(), "" );
                            ASSERT_BUG( sp, !ste.binding.is_Unbound(), "" );
                            if( te.binding.is_Opaque() || ste.binding.is_Opaque() ) {
                                // Either side is opaque, leave for now
                            }
                            else {
                                se.src = MIR_Cleanup_CoerceUnsized(state, mutator, e.type, src_ty, mv$(e.val));
                            }
                        )
                        else {
                            ASSERT_BUG( sp, src_ty.m_data.is_Generic(), "Cast to Path from " << src_ty );
                        }
                    )
                    else {
                    }
                }
            }
            
            DEBUG(it - block.statements.begin());
            it = mutator.flush();
            DEBUG(it - block.statements.begin());
            mutator.cur_stmt += 1;
        }
        
        state.set_cur_stmt_term( mutator.cur_block );
        TU_IFLET( ::MIR::Terminator, block.terminator, Call, e,
            
            TU_IFLET( ::MIR::CallTarget, e.fcn, Path, path,
                // Detect calling `<Trait as Trait>::method()` and replace with vtable call
                if( path.m_data.is_UfcsKnown() && path.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
                {
                    const auto& pe = path.m_data.as_UfcsKnown();
                    const auto& te = pe.type->m_data.as_TraitObject();
                    // TODO: What if the method is from a supertrait?

                    if( te.m_trait.m_path == pe.trait || resolve.find_named_trait_in_trait(
                            sp, pe.trait.m_path, pe.trait.m_params,
                            *te.m_trait.m_trait_ptr, te.m_trait.m_path.m_path, te.m_trait.m_path.m_params,
                            *pe.type,
                            [](const auto&, auto){}
                            )
                        )
                    {
                        auto tgt_lvalue = MIR_Cleanup_Virtualize(sp, state, mutator, e.args.front(), te, pe);
                        e.fcn = mv$(tgt_lvalue);
                    }
                }
            )
        )
        
        mutator.flush();
        mutator.cur_block += 1;
        mutator.cur_stmt = 0;
    }
}

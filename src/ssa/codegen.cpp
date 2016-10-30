struct ssaGen {
	ssaModule module;
	gbFile output_file;
};

b32 ssa_gen_init(ssaGen *s, Checker *c) {
	if (global_error_collector.count != 0) {
		return false;
	}

	isize tc = c->parser->total_token_count;
	if (tc < 2) {
		return false;
	}

	ssa_init_module(&s->module, c);
	s->module.generate_debug_info = false;

	// TODO(bill): generate appropriate output name
	int pos = cast(int)string_extension_position(c->parser->init_fullpath);
	gbFileError err = gb_file_create(&s->output_file, gb_bprintf("%.*s.ll", pos, c->parser->init_fullpath.text));
	if (err != gbFileError_None) {
		return false;
	}

	return true;
}

void ssa_gen_destroy(ssaGen *s) {
	ssa_destroy_module(&s->module);
	gb_file_close(&s->output_file);
}

String ssa_mangle_name(ssaGen *s, String path, String name) {
	// NOTE(bill): prefix names not in the init scope
	// TODO(bill): make robust and not just rely on the file's name

	ssaModule *m = &s->module;
	CheckerInfo *info = m->info;
	gbAllocator a = m->allocator;
	AstFile *file = *map_get(&info->files, hash_string(path));

	char *str = gb_alloc_array(a, char, path.len+1);
	gb_memmove(str, path.text, path.len);
	str[path.len] = 0;
	for (isize i = 0; i < path.len; i++) {
		if (str[i] == '\\') {
			str[i] = '/';
		}
	}

	char const *base = gb_path_base_name(str);
	char const *ext = gb_path_extension(base);
	isize base_len = ext-1-base;

	isize max_len = base_len + 1 + 10 + 1 + name.len;
	u8 *new_name = gb_alloc_array(a, u8, max_len);
	isize new_name_len = gb_snprintf(
		cast(char *)new_name, max_len,
		"%.*s-%u.%.*s",
		cast(int)base_len, base,
		file->id,
		LIT(name));

	return make_string(new_name, new_name_len-1);
}


void ssa_gen_tree(ssaGen *s) {
	ssaModule *m = &s->module;
	CheckerInfo *info = m->info;
	gbAllocator a = m->allocator;

	if (v_zero == NULL) {
		v_zero   = ssa_make_const_int (m->allocator, 0);
		v_one    = ssa_make_const_int (m->allocator, 1);
		v_zero32 = ssa_make_const_i32 (m->allocator, 0);
		v_one32  = ssa_make_const_i32 (m->allocator, 1);
		v_two32  = ssa_make_const_i32 (m->allocator, 2);
		v_false  = ssa_make_const_bool(m->allocator, false);
		v_true   = ssa_make_const_bool(m->allocator, true);
	}

	isize global_variable_max_count = 0;
	Entity *entry_point = NULL;

	for_array(i, info->entities.entries) {
		auto *entry = &info->entities.entries[i];
		Entity *e = cast(Entity *)cast(uintptr)entry->key.key;
		String name = e->token.string;
		if (e->kind == Entity_Variable) {
			global_variable_max_count++;
		} else if (e->kind == Entity_Procedure) {
			if (e->scope->is_init && name == "main") {
				entry_point = e;
			}
		}
	}

	struct ssaGlobalVariable {
		ssaValue *var, *init;
		DeclInfo *decl;
	};
	Array<ssaGlobalVariable> global_variables;
	array_init(&global_variables, m->tmp_allocator, global_variable_max_count);

	m->min_dep_map = generate_minimum_dependency_map(info, entry_point);

	for_array(i, info->entities.entries) {
		auto *entry = &info->entities.entries[i];
		Entity *e = cast(Entity *)entry->key.ptr;
		String name = e->token.string;
		DeclInfo *decl = entry->value;
		Scope *scope = e->scope;

		if (!scope->is_file) {
			continue;
		}

		if (map_get(&m->min_dep_map, hash_pointer(e)) == NULL) {
			// NOTE(bill): Nothing depends upon it so doesn't need to be built
			continue;
		}

		if (!scope->is_global && !scope->is_init) {
			name = ssa_mangle_name(s, e->token.pos.file, name);
		}


		switch (e->kind) {
		case Entity_TypeName:
			GB_ASSERT(e->type->kind == Type_Named);
			map_set(&m->type_names, hash_pointer(e->type), name);
			ssa_gen_global_type_name(m, e, name);
			break;

		case Entity_Variable: {
			ssaValue *g = ssa_make_value_global(a, e, NULL);
			if (decl->var_decl_tags & VarDeclTag_thread_local) {
				g->Global.is_thread_local = true;
			}
			ssaGlobalVariable var = {};
			var.var = g;
			var.decl = decl;

			if (decl->init_expr != NULL) {
				TypeAndValue *tav = map_get(&info->types, hash_pointer(decl->init_expr));
				if (tav != NULL) {
					if (tav->value.kind != ExactValue_Invalid) {
						ExactValue v = tav->value;
						// if (v.kind != ExactValue_String) {
							g->Global.value = ssa_add_module_constant(m, tav->type, v);
						// }
					}
				}
			}

			if (g->Global.value == NULL) {
				array_add(&global_variables, var);
			}

			map_set(&m->values, hash_pointer(e), g);
			map_set(&m->members, hash_string(name), g);
		} break;

		case Entity_Procedure: {
			auto *pd = &decl->proc_decl->ProcDecl;
			String original_name = name;
			AstNode *body = pd->body;
			if (pd->tags & ProcTag_foreign) {
				name = pd->name->Ident.string;
			}
			if (pd->foreign_name.len > 0) {
				name = pd->foreign_name;
			} else if (pd->link_name.len > 0) {
				name = pd->link_name;
			}

			ssaValue *p = ssa_make_value_procedure(a, m, e, e->type, decl->type_expr, body, name);
			p->Proc.tags = pd->tags;

			map_set(&m->values, hash_pointer(e), p);
			HashKey hash_name = hash_string(name);
			if (map_get(&m->members, hash_name) == NULL) {
				map_set(&m->members, hash_name, p);
			}
		} break;
		}
	}

	for_array(i, m->members.entries) {
		auto *entry = &m->members.entries[i];
		ssaValue *v = entry->value;
		if (v->kind == ssaValue_Proc)
			ssa_build_proc(v, NULL);
	}

	ssaDebugInfo *compile_unit = m->debug_info.entries[0].value;
	GB_ASSERT(compile_unit->kind == ssaDebugInfo_CompileUnit);
	ssaDebugInfo *all_procs = ssa_alloc_debug_info(m->allocator, ssaDebugInfo_AllProcs);

	isize all_proc_max_count = 0;
	for_array(i, m->debug_info.entries) {
		auto *entry = &m->debug_info.entries[i];
		ssaDebugInfo *di = entry->value;
		di->id = i;
		if (di->kind == ssaDebugInfo_Proc) {
			all_proc_max_count++;
		}
	}

	array_init(&all_procs->AllProcs.procs, m->allocator, all_proc_max_count);
	map_set(&m->debug_info, hash_pointer(all_procs), all_procs); // NOTE(bill): This doesn't need to be mapped
	compile_unit->CompileUnit.all_procs = all_procs;


	for_array(i, m->debug_info.entries) {
		auto *entry = &m->debug_info.entries[i];
		ssaDebugInfo *di = entry->value;
		di->id = i;
		if (di->kind == ssaDebugInfo_Proc) {
			array_add(&all_procs->AllProcs.procs, di);
		}
	}


	{ // Startup Runtime
		// Cleanup(bill): probably better way of doing code insertion
		String name = make_string(SSA_STARTUP_RUNTIME_PROC_NAME);
		Type *proc_type = make_type_proc(a, gb_alloc_item(a, Scope),
		                                 NULL, 0,
		                                 NULL, 0, false);
		AstNode *body = gb_alloc_item(a, AstNode);
		ssaValue *p = ssa_make_value_procedure(a, m, NULL, proc_type, NULL, body, name);
		Token token = {};
		token.string = name;
		Entity *e = make_entity_procedure(a, NULL, token, proc_type);

		map_set(&m->values, hash_pointer(e), p);
		map_set(&m->members, hash_string(name), p);

		ssaProcedure *proc = &p->Proc;
		proc->tags = ProcTag_no_inline; // TODO(bill): is no_inline a good idea?

		ssa_begin_procedure_body(proc);

		// TODO(bill): Should do a dependency graph do check which order to initialize them in?
		for_array(i, global_variables) {
			ssaGlobalVariable *var = &global_variables[i];
			if (var->decl->init_expr != NULL) {
				var->init = ssa_build_expr(proc, var->decl->init_expr);
			}
		}

		// NOTE(bill): Initialize constants first
		for_array(i, global_variables) {
			ssaGlobalVariable *var = &global_variables[i];
			if (var->init != NULL) {
				if (var->init->kind == ssaValue_Constant) {
					ssa_emit_store(proc, var->var, var->init);
				}
			}
		}

		for_array(i, global_variables) {
			ssaGlobalVariable *var = &global_variables[i];
			if (var->init != NULL) {
				if (var->init->kind != ssaValue_Constant) {
					ssa_emit_store(proc, var->var, var->init);
				}
			}
		}

		{ // NOTE(bill): Setup type_info data
			// TODO(bill): Try and make a lot of this constant aggregate literals in LLVM IR
			ssaValue *type_info_data = NULL;
			ssaValue *type_info_member_data = NULL;

			ssaValue **found = NULL;
			found = map_get(&proc->module->members, hash_string(make_string(SSA_TYPE_INFO_DATA_NAME)));
			GB_ASSERT(found != NULL);
			type_info_data = *found;

			found = map_get(&proc->module->members, hash_string(make_string(SSA_TYPE_INFO_DATA_MEMBER_NAME)));
			GB_ASSERT(found != NULL);
			type_info_member_data = *found;

			CheckerInfo *info = proc->module->info;

			// Useful types
			Type *t_i64_slice_ptr    = make_type_pointer(a, make_type_slice(a, t_i64));
			Type *t_string_slice_ptr = make_type_pointer(a, make_type_slice(a, t_string));

			auto get_type_info_ptr = [](ssaProcedure *proc, ssaValue *type_info_data, Type *type) -> ssaValue * {
				i32 index = cast(i32)ssa_type_info_index(proc->module->info, type);
				// gb_printf_err("%d %s\n", index, type_to_string(type));
				return ssa_emit_array_ep(proc, type_info_data, index);
			};

			i32 type_info_member_index = 0;

			auto type_info_member_offset = [](ssaProcedure *proc, ssaValue *data, isize count, i32 *index) -> ssaValue * {
				ssaValue *offset = ssa_emit_array_ep(proc, data, *index);
				*index += count;
				return offset;
			};



			for_array(type_info_map_index, info->type_info_map.entries) {
				auto *entry = &info->type_info_map.entries[type_info_map_index];
				Type *t = cast(Type *)cast(uintptr)entry->key.key;
				t = default_type(t);
				isize entry_index = entry->value;

				ssaValue *tag = NULL;

				switch (t->kind) {
				case Type_Named: {
					tag = ssa_add_local_generated(proc, t_type_info_named);

					// TODO(bill): Which is better? The mangled name or actual name?
					ssaValue *name = ssa_make_const_string(a, t->Named.type_name->token.string);
					ssaValue *gtip = get_type_info_ptr(proc, type_info_data, t->Named.base);

					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), name);
					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 1), gtip);
				} break;

				case Type_Basic:
					switch (t->Basic.kind) {
					case Basic_bool:
						tag = ssa_add_local_generated(proc, t_type_info_boolean);
						break;
					case Basic_i8:
					case Basic_i16:
					case Basic_i32:
					case Basic_i64:
					case Basic_u8:
					case Basic_u16:
					case Basic_u32:
					case Basic_u64:
					case Basic_int:
					case Basic_uint: {
						tag = ssa_add_local_generated(proc, t_type_info_integer);
						b32 is_unsigned = (t->Basic.flags & BasicFlag_Unsigned) != 0;
						ssaValue *bits = ssa_make_const_int(a, type_size_of(m->sizes, a, t));
						ssaValue *is_signed = ssa_make_const_bool(a, !is_unsigned);
						ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), bits);
						ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 1), is_signed);
					} break;

					case Basic_f32:
					case Basic_f64: {
						tag = ssa_add_local_generated(proc, t_type_info_float);
						ssaValue *bits = ssa_make_const_int(a, type_size_of(m->sizes, a, t));
						ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), bits);
					} break;

					case Basic_rawptr:
						tag = ssa_add_local_generated(proc, t_type_info_pointer);
						break;

					case Basic_string:
						tag = ssa_add_local_generated(proc, t_type_info_string);
						break;

					case Basic_any:
						tag = ssa_add_local_generated(proc, t_type_info_any);
						break;
					}
					break;

				case Type_Pointer: {
					tag = ssa_add_local_generated(proc, t_type_info_pointer);
					ssaValue *gep = get_type_info_ptr(proc, type_info_data, t->Pointer.elem);
					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), gep);
				} break;
				case Type_Maybe: {
					tag = ssa_add_local_generated(proc, t_type_info_maybe);
					ssaValue *gep = get_type_info_ptr(proc, type_info_data, t->Maybe.elem);
					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), gep);
				} break;
				case Type_Array: {
					tag = ssa_add_local_generated(proc, t_type_info_array);
					ssaValue *gep = get_type_info_ptr(proc, type_info_data, t->Array.elem);
					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), gep);

					isize ez = type_size_of(m->sizes, a, t->Array.elem);
					ssaValue *elem_size = ssa_emit_struct_ep(proc, tag, 1);
					ssa_emit_store(proc, elem_size, ssa_make_const_int(a, ez));

					ssaValue *count = ssa_emit_struct_ep(proc, tag, 2);
					ssa_emit_store(proc, count, ssa_make_const_int(a, t->Array.count));

				} break;
				case Type_Slice: {
					tag = ssa_add_local_generated(proc, t_type_info_slice);
					ssaValue *gep = get_type_info_ptr(proc, type_info_data, t->Slice.elem);
					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), gep);

					isize ez = type_size_of(m->sizes, a, t->Slice.elem);
					ssaValue *elem_size = ssa_emit_struct_ep(proc, tag, 1);
					ssa_emit_store(proc, elem_size, ssa_make_const_int(a, ez));

				} break;
				case Type_Vector: {
					tag = ssa_add_local_generated(proc, t_type_info_vector);
					ssaValue *gep = get_type_info_ptr(proc, type_info_data, t->Vector.elem);
					ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 0), gep);

					isize ez = type_size_of(m->sizes, a, t->Vector.elem);
					ssaValue *elem_size = ssa_emit_struct_ep(proc, tag, 1);
					ssa_emit_store(proc, elem_size, ssa_make_const_int(a, ez));

					ssaValue *count = ssa_emit_struct_ep(proc, tag, 2);
					ssa_emit_store(proc, count, ssa_make_const_int(a, t->Vector.count));

				} break;
				case Type_Record: {
					switch (t->Record.kind) {
					case TypeRecord_Struct: {
						tag = ssa_add_local_generated(proc, t_type_info_struct);

						{
							ssaValue *packed  = ssa_make_const_bool(a, t->Record.struct_is_packed);
							ssaValue *ordered = ssa_make_const_bool(a, t->Record.struct_is_ordered);
							ssaValue *size    = ssa_make_const_int(a, type_size_of(m->sizes, a, t));
							ssaValue *align   = ssa_make_const_int(a, type_align_of(m->sizes, a, t));
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 1), size);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 2), align);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 3), packed);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 4), ordered);
						}

						ssaValue *memory = type_info_member_offset(proc, type_info_member_data, t->Record.field_count, &type_info_member_index);

						type_set_offsets(m->sizes, a, t); // NOTE(bill): Just incase the offsets have not been set yet
						for (isize source_index = 0; source_index < t->Record.field_count; source_index++) {
							// TODO(bill): Order fields in source order not layout order
							Entity *f = t->Record.fields_in_src_order[source_index];
							ssaValue *tip = get_type_info_ptr(proc, type_info_data, f->type);
							i64 foffset = t->Record.struct_offsets[f->Variable.field_index];
							GB_ASSERT(f->kind == Entity_Variable && f->flags & EntityFlag_Field);

							ssaValue *field     = ssa_emit_ptr_offset(proc, memory, ssa_make_const_int(a, source_index));
							ssaValue *name      = ssa_emit_struct_ep(proc, field, 0);
							ssaValue *type_info = ssa_emit_struct_ep(proc, field, 1);
							ssaValue *offset    = ssa_emit_struct_ep(proc, field, 2);

							if (f->token.string.len > 0) {
								ssa_emit_store(proc, name, ssa_make_const_string(a, f->token.string));
							}
							ssa_emit_store(proc, type_info, tip);
							ssa_emit_store(proc, offset, ssa_make_const_int(a, foffset));
						}

						Type *slice_type = make_type_slice(a, t_type_info_member);
						Type *slice_type_ptr = make_type_pointer(a, slice_type);
						ssaValue *slice = ssa_emit_struct_ep(proc, tag, 0);
						ssaValue *field_count = ssa_make_const_int(a, t->Record.field_count);

						ssaValue *elem = ssa_emit_struct_ep(proc, slice, 0);
						ssaValue *len  = ssa_emit_struct_ep(proc, slice, 1);
						ssaValue *cap  = ssa_emit_struct_ep(proc, slice, 2);

						ssa_emit_store(proc, elem, memory);
						ssa_emit_store(proc, len, field_count);
						ssa_emit_store(proc, cap, field_count);
					} break;
					case TypeRecord_Union:
						tag = ssa_add_local_generated(proc, t_type_info_union);
						{
							ssaValue *size    = ssa_make_const_int(a, type_size_of(m->sizes, a, t));
							ssaValue *align   = ssa_make_const_int(a, type_align_of(m->sizes, a, t));
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 1),  size);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 2),  align);
						}
						break;
					case TypeRecord_RawUnion: {
						tag = ssa_add_local_generated(proc, t_type_info_raw_union);
						{
							ssaValue *size    = ssa_make_const_int(a, type_size_of(m->sizes, a, t));
							ssaValue *align   = ssa_make_const_int(a, type_align_of(m->sizes, a, t));
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 1),  size);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 2),  align);
						}

						ssaValue *memory = type_info_member_offset(proc, type_info_member_data, t->Record.field_count, &type_info_member_index);

						for (isize i = 0; i < t->Record.field_count; i++) {
							ssaValue *field     = ssa_emit_ptr_offset(proc, memory, ssa_make_const_int(a, i));
							ssaValue *name      = ssa_emit_struct_ep(proc, field, 0);
							ssaValue *type_info = ssa_emit_struct_ep(proc, field, 1);
							ssaValue *offset    = ssa_emit_struct_ep(proc, field, 2);

							Entity *f = t->Record.fields[i];
							ssaValue *tip = get_type_info_ptr(proc, type_info_data, f->type);

							if (f->token.string.len > 0) {
								ssa_emit_store(proc, name, ssa_make_const_string(a, f->token.string));
							}
							ssa_emit_store(proc, type_info, tip);
							ssa_emit_store(proc, offset, ssa_make_const_int(a, 0));
						}

						Type *slice_type = make_type_slice(a, t_type_info_member);
						Type *slice_type_ptr = make_type_pointer(a, slice_type);
						ssaValue *slice = ssa_emit_struct_ep(proc, tag, 0);
						ssaValue *field_count = ssa_make_const_int(a, t->Record.field_count);

						ssaValue *elem = ssa_emit_struct_ep(proc, slice, 0);
						ssaValue *len  = ssa_emit_struct_ep(proc, slice, 1);
						ssaValue *cap  = ssa_emit_struct_ep(proc, slice, 2);

						ssa_emit_store(proc, elem, memory);
						ssa_emit_store(proc, len, field_count);
						ssa_emit_store(proc, cap, field_count);
					} break;
					case TypeRecord_Enum: {
						tag = ssa_add_local_generated(proc, t_type_info_enum);
						Type *enum_base = t->Record.enum_base;
						if (enum_base == NULL) {
							enum_base = t_int;
						}
						ssaValue *base = ssa_emit_struct_ep(proc, tag, 0);
						ssa_emit_store(proc, base, get_type_info_ptr(proc, type_info_data, enum_base));

						if (t->Record.other_field_count > 0) {
							Entity **fields = t->Record.other_fields;
							isize count = t->Record.other_field_count;
							ssaValue *value_array = NULL;
							ssaValue *name_array = NULL;


							{
								Token token = {Token_Identifier};
								i32 id = cast(i32)entry_index;
								char name_base[] = "__$enum_values";
								isize name_len = gb_size_of(name_base) + 10;
								token.string.text = gb_alloc_array(a, u8, name_len);
								token.string.len = gb_snprintf(cast(char *)token.string.text, name_len,
								                               "%s-%d", name_base, id)-1;
								Entity *e = make_entity_variable(a, NULL, token, make_type_array(a, t_i64, count));
								value_array = ssa_make_value_global(a, e, NULL);
								value_array->Global.is_private = true;
								ssa_module_add_value(m, e, value_array);
								map_set(&m->members, hash_string(token.string), value_array);
							}
							{
								Token token = {Token_Identifier};
								i32 id = cast(i32)entry_index;
								char name_base[] = "__$enum_names";
								isize name_len = gb_size_of(name_base) + 10;
								token.string.text = gb_alloc_array(a, u8, name_len);
								token.string.len = gb_snprintf(cast(char *)token.string.text, name_len,
								                               "%s-%d", name_base, id)-1;
								Entity *e = make_entity_variable(a, NULL, token, make_type_array(a, t_string, count));
								name_array = ssa_make_value_global(a, e, NULL);
								name_array->Global.is_private = true;
								ssa_module_add_value(m, e, name_array);
								map_set(&m->members, hash_string(token.string), name_array);
							}

							for (isize i = 0; i < count; i++) {
								ssaValue *value_gep = ssa_emit_struct_ep(proc, value_array, i);
								ssaValue *name_gep  = ssa_emit_struct_ep(proc, name_array, i);

								ssa_emit_store(proc, value_gep, ssa_make_const_i64(a, fields[i]->Constant.value.value_integer));
								ssa_emit_store(proc, name_gep,  ssa_make_const_string(a, fields[i]->token.string));
							}

							ssaValue *v_count = ssa_make_const_int(a, count);


							ssaValue *values = ssa_emit_struct_ep(proc, tag, 1);
							ssaValue *names  = ssa_emit_struct_ep(proc, tag, 2);
							ssaValue *value_slice = ssa_add_local_generated(proc, type_deref(t_i64_slice_ptr));
							ssaValue *name_slice  = ssa_add_local_generated(proc, type_deref(t_string_slice_ptr));

							ssa_emit_store(proc, ssa_emit_struct_ep(proc, value_slice, 0), ssa_array_elem(proc, value_array));
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, value_slice, 1), v_count);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, value_slice, 2), v_count);

							ssa_emit_store(proc, ssa_emit_struct_ep(proc, name_slice, 0), ssa_array_elem(proc, name_array));
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, name_slice, 1), v_count);
							ssa_emit_store(proc, ssa_emit_struct_ep(proc, name_slice, 2), v_count);

							ssa_emit_store(proc, values, ssa_emit_load(proc, value_slice));
							ssa_emit_store(proc, names,  ssa_emit_load(proc, name_slice));
						}
					} break;
					}
				} break;

				case Type_Tuple: {
					tag = ssa_add_local_generated(proc, t_type_info_tuple);

					{
						ssaValue *align = ssa_make_const_int(a, type_align_of(m->sizes, a, t));
						ssa_emit_store(proc, ssa_emit_struct_ep(proc, tag, 2), align);
					}

					ssaValue *memory = type_info_member_offset(proc, type_info_member_data, t->Tuple.variable_count, &type_info_member_index);

					for (isize i = 0; i < t->Tuple.variable_count; i++) {
						ssaValue *field     = ssa_emit_ptr_offset(proc, memory, ssa_make_const_int(a, i));
						ssaValue *name      = ssa_emit_struct_ep(proc, field, 0);
						ssaValue *type_info = ssa_emit_struct_ep(proc, field, 1);
						// NOTE(bill): offset is not used for tuples

						Entity *f = t->Tuple.variables[i];
						ssaValue *tip = get_type_info_ptr(proc, type_info_data, f->type);

						if (f->token.string.len > 0) {
							ssa_emit_store(proc, name, ssa_make_const_string(a, f->token.string));
						}
						ssa_emit_store(proc, type_info, tip);
					}

					Type *slice_type = make_type_slice(a, t_type_info_member);
					Type *slice_type_ptr = make_type_pointer(a, slice_type);
					ssaValue *slice = ssa_emit_struct_ep(proc, tag, 0);
					ssaValue *variable_count = ssa_make_const_int(a, t->Tuple.variable_count);

					ssaValue *elem = ssa_emit_struct_ep(proc, slice, 0);
					ssaValue *len  = ssa_emit_struct_ep(proc, slice, 1);
					ssaValue *cap  = ssa_emit_struct_ep(proc, slice, 2);

					ssa_emit_store(proc, elem, memory);
					ssa_emit_store(proc, len, variable_count);
					ssa_emit_store(proc, cap, variable_count);
				} break;

				case Type_Proc: {
					tag = ssa_add_local_generated(proc, t_type_info_procedure);

					ssaValue *params   = ssa_emit_struct_ep(proc, tag, 0);
					ssaValue *results  = ssa_emit_struct_ep(proc, tag, 1);
					ssaValue *variadic = ssa_emit_struct_ep(proc, tag, 2);

					if (t->Proc.params) {
						ssa_emit_store(proc, params, get_type_info_ptr(proc, type_info_data, t->Proc.params));
					}
					if (t->Proc.results) {
						ssa_emit_store(proc, results, get_type_info_ptr(proc, type_info_data, t->Proc.results));
					}
					ssa_emit_store(proc, variadic, ssa_make_const_bool(a, t->Proc.variadic));

					// TODO(bill): Type_Info for procedures
				} break;
				}

				if (tag != NULL) {
					ssaValue *gep = ssa_emit_array_ep(proc, type_info_data, entry_index);
					ssaValue *val = ssa_emit_conv(proc, ssa_emit_load(proc, tag), t_type_info);
					ssa_emit_store(proc, gep, val);
				}
			}
		}

		ssa_end_procedure_body(proc);
	}

	for_array(i, m->procs) {
		ssa_build_proc(m->procs[i], m->procs[i]->Proc.parent);
	}


	// m->layout = make_string("e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64");
}

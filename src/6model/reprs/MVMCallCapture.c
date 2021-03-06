#include "moar.h"

/* This representation's function pointer table. */
static const MVMREPROps MVMCallCapture_this_repr;

/* Creates a new type object of this representation, and associates it with
 * the given HOW. Also sets the invocation protocol handler in the STable. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable *st = MVM_gc_allocate_stable(tc, &MVMCallCapture_this_repr, HOW);

    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);
        MVM_ASSIGN_REF(tc, &(st->header), st->WHAT, obj);
        st->size = sizeof(MVMCallCapture);
    });

    return st->WHAT;
}

/* Copies the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, MVMObject *dest_root, void *dest) {
    MVMCallCaptureBody *src_body  = (MVMCallCaptureBody *)src;
    MVMCallCaptureBody *dest_body = (MVMCallCaptureBody *)dest;

    MVMuint32 arg_size = src_body->apc->arg_count * sizeof(MVMRegister);
    MVMRegister *args = MVM_malloc(arg_size);
    memcpy(args, src_body->apc->args, arg_size);

    dest_body->apc = (MVMArgProcContext *)MVM_calloc(1, sizeof(MVMArgProcContext));
    dest_body->mode = MVM_CALL_CAPTURE_MODE_SAVE;

    if (src_body->owns_callsite) {
        dest_body->owns_callsite = 1;
        dest_body->effective_callsite = MVM_args_copy_callsite(tc, src_body->apc);
    }
    else {
        dest_body->owns_callsite = 0;
        dest_body->effective_callsite = src_body->effective_callsite;
    }
    MVM_args_proc_init(tc, dest_body->apc, dest_body->effective_callsite, args);
}

/* Adds held objects to the GC worklist. */
static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data, MVMGCWorklist *worklist) {
    /* Only need to worry about the SAVE case, as the USE case will be marked by
     * the frame holding the args being marked. */
    MVMCallCaptureBody *body = (MVMCallCaptureBody *)data;
    if (body->mode == MVM_CALL_CAPTURE_MODE_SAVE) {
        MVMArgProcContext *ctx = body->apc;
        MVMuint8  *flag_map = ctx->arg_flags ? ctx->arg_flags : ctx->callsite->arg_flags;
        MVMuint16  count = ctx->arg_count;
        MVMuint16  i, flag;
        for (i = 0, flag = 0; i < count; i++, flag++) {
            if (flag_map[flag] & MVM_CALLSITE_ARG_NAMED) {
                /* Current position is name, then next is value. */
                MVM_gc_worklist_add(tc, worklist, &ctx->args[i].s);
                i++;
            }
            if (flag_map[flag] & MVM_CALLSITE_ARG_STR || flag_map[flag] & MVM_CALLSITE_ARG_OBJ)
                MVM_gc_worklist_add(tc, worklist, &ctx->args[i].o);
        }
    }
    else {
        MVM_gc_worklist_add(tc, worklist, &(body->use_mode_frame));
    }
}

/* Called by the VM in order to free memory associated with this object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    MVMCallCapture *ctx = (MVMCallCapture *)obj;
    if (ctx->body.apc && ctx->body.effective_callsite != ctx->body.apc->callsite) {
        MVM_free(ctx->body.effective_callsite->arg_flags);
        MVM_free(ctx->body.effective_callsite);
    }
    else if (ctx->body.owns_callsite) {
        MVM_free(ctx->body.effective_callsite->arg_flags);
        MVM_free(ctx->body.effective_callsite);
    }
    if (ctx->body.mode == MVM_CALL_CAPTURE_MODE_SAVE) {
        /* We made our own copy of the args buffer and processing context, so
         * free them both. */
        if (ctx->body.apc) {
            if (ctx->body.apc->named_used) {
                MVM_fixed_size_free(tc, tc->instance->fsa,
                    ctx->body.apc->named_used_size,
                    ctx->body.apc->named_used);
                ctx->body.apc->named_used = NULL;
            }
            MVM_free(ctx->body.apc->args);
            MVM_free(ctx->body.apc);
        }
    }
}


static const MVMStorageSpec storage_spec = {
    MVM_STORAGE_SPEC_REFERENCE, /* inlineable */
    0,                          /* bits */
    0,                          /* align */
    MVM_STORAGE_SPEC_BP_NONE,   /* boxed_primitive */
    0,                          /* can_box */
    0,                          /* is_unsigned */
};

/* Gets the storage specification for this representation. */
static const MVMStorageSpec * get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    return &storage_spec;
}

/* Compose the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *info) {
    /* Nothing to do for this REPR. */
}

/* Initializes the representation. */
const MVMREPROps * MVMCallCapture_initialize(MVMThreadContext *tc) {
    return &MVMCallCapture_this_repr;
}

static const MVMREPROps MVMCallCapture_this_repr = {
    type_object_for,
    MVM_gc_allocate_object,
    NULL, /* initialize */
    copy_to,
    MVM_REPR_DEFAULT_ATTR_FUNCS,
    MVM_REPR_DEFAULT_BOX_FUNCS,
    MVM_REPR_DEFAULT_POS_FUNCS,
    MVM_REPR_DEFAULT_ASS_FUNCS,
    MVM_REPR_DEFAULT_ELEMS,
    get_storage_spec,
    NULL, /* change_type */
    NULL, /* serialize */
    NULL, /* deserialize */
    NULL, /* serialize_repr_data */
    NULL, /* deserialize_repr_data */
    NULL, /* deserialize_stable_size */
    gc_mark,
    gc_free,
    NULL, /* gc_cleanup */
    NULL, /* gc_mark_repr_data */
    NULL, /* gc_free_repr_data */
    compose,
    NULL, /* spesh */
    "MVMCallCapture", /* name */
    MVM_REPR_ID_MVMCallCapture,
    NULL, /* unmanaged_size */
    NULL, /* describe_refs */
};

/* This function was only introduced for the benefit of the JIT. */
MVMint64 MVM_capture_pos_primspec(MVMThreadContext *tc, MVMObject *obj, MVMint64 i) {
    if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
        MVMCallCapture *cc = (MVMCallCapture *)obj;
        if (i >= 0 && i < cc->body.apc->num_pos) {
            MVMCallsiteEntry *arg_flags = cc->body.apc->arg_flags
                ? cc->body.apc->arg_flags
                : cc->body.apc->callsite->arg_flags;
            switch (arg_flags[i] & MVM_CALLSITE_ARG_MASK) {
                case MVM_CALLSITE_ARG_INT:
                    return MVM_STORAGE_SPEC_BP_INT;
                case MVM_CALLSITE_ARG_NUM:
                    return MVM_STORAGE_SPEC_BP_NUM;
                case MVM_CALLSITE_ARG_STR:
                    return MVM_STORAGE_SPEC_BP_STR;
                default:
                    return MVM_STORAGE_SPEC_BP_NONE;
            }
        }
        else {
            MVM_exception_throw_adhoc(tc,
                "Bad argument index given to captureposprimspec");
        }
    }
    else {
        MVM_exception_throw_adhoc(tc, "captureposprimspec needs a MVMCallCapture");
    }
}

/*
 * Riz Programming Language
 * value.c — Runtime value operations (Phase 3: +struct/instance)
 */

#include "value.h"

/* ═══ Constructors ═══ */

RizValue riz_int(int64_t v)    { RizValue r; r.type=VAL_INT;   r.as.integer=v;  return r; }
RizValue riz_float(double v)   { RizValue r; r.type=VAL_FLOAT; r.as.floating=v; return r; }
RizValue riz_bool(bool v)      { RizValue r; r.type=VAL_BOOL;  r.as.boolean=v;  return r; }
RizValue riz_none(void)        { RizValue r; r.type=VAL_NONE;  r.as.integer=0;  return r; }
RizValue riz_string(const char* v) { RizValue r; r.type=VAL_STRING; r.as.string=riz_strdup(v); return r; }
RizValue riz_string_take(char* v) { RizValue r; r.type=VAL_STRING; r.as.string=v; return r; }
RizValue riz_fn(RizFunction* fn) { RizValue r; r.type=VAL_FUNCTION; r.as.function=fn; return r; }
RizValue riz_native(const char* name, NativeFnPtr fn, int arity) {
    NativeFnObj* obj=RIZ_ALLOC(NativeFnObj); obj->name=riz_strdup(name); obj->fn=fn; obj->arity=arity;
    RizValue r; r.type=VAL_NATIVE_FN; r.as.native=obj; return r;
}
RizValue riz_list_new(void) { RizList* l=RIZ_ALLOC(RizList); l->ref_count=1; RizValue r; r.type=VAL_LIST; r.as.list=l; return r; }
RizValue riz_dict_new(void) {
    RizDict* d=RIZ_ALLOC(RizDict); d->ref_count=1;
    d->hash_capacity = 8;
    d->hash_table = (int*)malloc(sizeof(int) * d->hash_capacity);
    for(int i=0;i<d->hash_capacity;i++) d->hash_table[i] = -1;
    RizValue r; r.type=VAL_DICT; r.as.dict=d; return r;
}

RizValue riz_struct_def_new(const char* name, char** fields, int field_count) {
    RizStructDef* def = RIZ_ALLOC(RizStructDef);
    def->name = riz_strdup(name);
    def->field_names = fields; /* takes ownership */
    def->field_count = field_count;
    def->method_names = NULL; def->method_values = NULL;
    def->method_count = 0; def->method_cap = 0;
    RizValue r; r.type = VAL_STRUCT_DEF; r.as.struct_def = def; return r;
}

RizValue riz_instance_new(RizStructDef* def, RizValue* field_values) {
    RizInstance* inst = RIZ_ALLOC(RizInstance);
    inst->def = def; inst->ref_count = 1;
    inst->fields = RIZ_ALLOC_ARRAY(RizValue, def->field_count);
    for (int i = 0; i < def->field_count; i++) inst->fields[i] = field_values[i];
    RizValue r; r.type = VAL_INSTANCE; r.as.instance = inst; return r;
}

/* ═══ Struct methods ═══ */

void riz_struct_add_method(RizStructDef* def, const char* name, RizValue fn_val) {
    if (def->method_count >= def->method_cap) {
        int nc = def->method_cap < 8 ? 8 : def->method_cap * 2;
        def->method_names  = RIZ_GROW_ARRAY(char*,    def->method_names,  def->method_cap, nc);
        def->method_values = RIZ_GROW_ARRAY(RizValue, def->method_values, def->method_cap, nc);
        def->method_cap = nc;
    }
    def->method_names[def->method_count] = riz_strdup(name);
    def->method_values[def->method_count] = fn_val;
    def->method_count++;
}

/* ═══ Print ═══ */

void riz_value_print(RizValue v) {
    switch (v.type) {
        case VAL_INT:    printf("%lld", (long long)v.as.integer); break;
        case VAL_FLOAT: { double d=v.as.floating; if(d==(int64_t)d&&fabs(d)<1e15)printf("%.1f",d); else printf("%g",d); break; }
        case VAL_BOOL:   printf("%s", v.as.boolean?"true":"false"); break;
        case VAL_STRING: printf("%s", v.as.string); break;
        case VAL_NONE:   printf("none"); break;
        case VAL_FUNCTION: printf("<fn %s>", v.as.function->name?v.as.function->name:"anonymous"); break;
        case VAL_NATIVE_FN: printf("<builtin %s>", v.as.native->name); break;
        case VAL_LIST: {
            printf("["); for(int i=0;i<v.as.list->count;i++){if(i>0)printf(", ");
            if(v.as.list->items[i].type==VAL_STRING)printf("\"%s\"",v.as.list->items[i].as.string);else riz_value_print(v.as.list->items[i]);}printf("]"); break;
        }
        case VAL_DICT: {
            RizDict*d=v.as.dict; printf("{"); for(int i=0;i<d->count;i++){if(i>0)printf(", ");printf("%s: ",d->keys[i]);
            if(d->values[i].type==VAL_STRING)printf("\"%s\"",d->values[i].as.string);else riz_value_print(d->values[i]);}printf("}"); break;
        }
        case VAL_STRUCT_DEF: printf("<struct %s>", v.as.struct_def->name); break;
        case VAL_TRAIT_DEF: printf("<trait %s>", v.as.trait_def->name); break;
        case VAL_INSTANCE: {
            RizInstance* inst=v.as.instance; printf("%s(", inst->def->name);
            for(int i=0;i<inst->def->field_count;i++){if(i>0)printf(", ");printf("%s=",inst->def->field_names[i]);riz_value_print(inst->fields[i]);}
            printf(")"); break;
        }
    }
}

char* riz_value_to_string(RizValue v) {
    char buf[256];
    switch (v.type) {
        case VAL_INT: snprintf(buf,sizeof(buf),"%lld",(long long)v.as.integer); return riz_strdup(buf);
        case VAL_FLOAT: if(v.as.floating==(int64_t)v.as.floating&&fabs(v.as.floating)<1e15)snprintf(buf,sizeof(buf),"%.1f",v.as.floating); else snprintf(buf,sizeof(buf),"%g",v.as.floating); return riz_strdup(buf);
        case VAL_BOOL: return riz_strdup(v.as.boolean?"true":"false");
        case VAL_STRING: return riz_strdup(v.as.string);
        case VAL_NONE: return riz_strdup("none");
        case VAL_FUNCTION: snprintf(buf,sizeof(buf),"<fn %s>",v.as.function->name?v.as.function->name:"anon"); return riz_strdup(buf);
        case VAL_NATIVE_FN: snprintf(buf,sizeof(buf),"<builtin %s>",v.as.native->name); return riz_strdup(buf);
        case VAL_STRUCT_DEF: snprintf(buf,sizeof(buf),"<struct %s>",v.as.struct_def->name); return riz_strdup(buf);
        case VAL_TRAIT_DEF: snprintf(buf,sizeof(buf),"<trait %s>",v.as.trait_def->name); return riz_strdup(buf);
        case VAL_INSTANCE: {
            size_t cap=128;char*res=(char*)malloc(cap);size_t len=0;
            const char*nm=v.as.instance->def->name;size_t nl=strlen(nm);
            memcpy(res,nm,nl);len+=nl;res[len++]='(';
            for(int i=0;i<v.as.instance->def->field_count;i++){
                if(i>0){while(len+3>=cap){cap*=2;res=(char*)realloc(res,cap);}res[len++]=',';res[len++]=' ';}
                const char*fn_nm=v.as.instance->def->field_names[i];size_t fnl=strlen(fn_nm);
                while(len+fnl+4>=cap){cap*=2;res=(char*)realloc(res,cap);}
                memcpy(res+len,fn_nm,fnl);len+=fnl;res[len++]='=';
                char*vs=riz_value_to_string(v.as.instance->fields[i]);size_t vl=strlen(vs);
                while(len+vl+4>=cap){cap*=2;res=(char*)realloc(res,cap);}
                memcpy(res+len,vs,vl);len+=vl;free(vs);
            }
            while(len+2>=cap){cap*=2;res=(char*)realloc(res,cap);}
            res[len++]=')';res[len]='\0';return res;
        }
        case VAL_LIST: case VAL_DICT: {
            size_t cap=128;char*res=(char*)malloc(cap);size_t len=0;
            res[len++]=v.type==VAL_LIST?'[':'{';
            int cnt=v.type==VAL_LIST?v.as.list->count:v.as.dict->count;
            for(int i=0;i<cnt;i++){
                if(i>0){while(len+2>=cap){cap*=2;res=(char*)realloc(res,cap);}res[len++]=',';res[len++]=' ';}
                if(v.type==VAL_DICT){const char*k=v.as.dict->keys[i];size_t kl=strlen(k);while(len+kl+4>=cap){cap*=2;res=(char*)realloc(res,cap);}memcpy(res+len,k,kl);len+=kl;res[len++]=':';res[len++]=' ';}
                RizValue item=v.type==VAL_LIST?v.as.list->items[i]:v.as.dict->values[i];
                char*s=riz_value_to_string(item);size_t sl=strlen(s);bool q=item.type==VAL_STRING;
                while(len+sl+4>=cap){cap*=2;res=(char*)realloc(res,cap);}
                if(q)res[len++]='"';memcpy(res+len,s,sl);len+=sl;if(q)res[len++]='"';free(s);
            }
            while(len+2>=cap){cap*=2;res=(char*)realloc(res,cap);}
            res[len++]=v.type==VAL_LIST?']':'}';res[len]='\0';return res;
        }
    }
    return riz_strdup("?");
}

/* ═══ Type checking ═══ */

bool riz_value_is_truthy(RizValue v) {
    switch(v.type){case VAL_BOOL:return v.as.boolean;case VAL_NONE:return false;case VAL_INT:return v.as.integer!=0;
    case VAL_FLOAT:return v.as.floating!=0.0;case VAL_STRING:return v.as.string[0]!='\0';case VAL_LIST:return v.as.list->count>0;
    case VAL_DICT:return v.as.dict->count>0;default:return true;}
}

bool riz_value_equal(RizValue a, RizValue b) {
    if(a.type!=b.type){if(a.type==VAL_INT&&b.type==VAL_FLOAT)return(double)a.as.integer==b.as.floating;if(a.type==VAL_FLOAT&&b.type==VAL_INT)return a.as.floating==(double)b.as.integer;return false;}
    switch(a.type){
        case VAL_INT:return a.as.integer==b.as.integer;case VAL_FLOAT:return a.as.floating==b.as.floating;
        case VAL_BOOL:return a.as.boolean==b.as.boolean;case VAL_STRING:return strcmp(a.as.string,b.as.string)==0;
        case VAL_NONE:return true;
        case VAL_LIST:{if(a.as.list->count!=b.as.list->count)return false;for(int i=0;i<a.as.list->count;i++)if(!riz_value_equal(a.as.list->items[i],b.as.list->items[i]))return false;return true;}
        case VAL_INSTANCE: return a.as.instance==b.as.instance; /* identity */
        default:return false;
    }
}

const char* riz_value_type_name(RizValue v) {
    switch(v.type){case VAL_INT:return"int";case VAL_FLOAT:return"float";case VAL_BOOL:return"bool";case VAL_STRING:return"str";
    case VAL_NONE:return"none";case VAL_FUNCTION:return"function";case VAL_NATIVE_FN:return"builtin";
    case VAL_LIST:return"list";case VAL_DICT:return"dict";
    case VAL_STRUCT_DEF:return v.as.struct_def->name;
    case VAL_TRAIT_DEF:return v.as.trait_def->name;
    case VAL_INSTANCE:return v.as.instance->def->name;}
    return"unknown";
}

/* ═══ Copy & Free ═══ */

RizValue riz_value_copy(RizValue v) {
    if(v.type==VAL_STRING)return riz_string(v.as.string);
    if(v.type==VAL_LIST){v.as.list->ref_count++;return v;}
    if(v.type==VAL_DICT){v.as.dict->ref_count++;return v;}
    if(v.type==VAL_INSTANCE){v.as.instance->ref_count++;return v;}
    return v;
}

void riz_value_free(RizValue* v) {
    if(!v)return;
    switch(v->type){
        case VAL_STRING:free(v->as.string);v->as.string=NULL;break;
        case VAL_LIST:if(v->as.list&&--v->as.list->ref_count<=0){for(int i=0;i<v->as.list->count;i++)riz_value_free(&v->as.list->items[i]);free(v->as.list->items);free(v->as.list);}v->as.list=NULL;break;
        case VAL_DICT:if(v->as.dict&&--v->as.dict->ref_count<=0){for(int i=0;i<v->as.dict->count;i++){free(v->as.dict->keys[i]);riz_value_free(&v->as.dict->values[i]);}free(v->as.dict->keys);free(v->as.dict->values);free(v->as.dict->hash_table);free(v->as.dict);}v->as.dict=NULL;break;
        case VAL_INSTANCE:if(v->as.instance&&--v->as.instance->ref_count<=0){for(int i=0;i<v->as.instance->def->field_count;i++)riz_value_free(&v->as.instance->fields[i]);free(v->as.instance->fields);free(v->as.instance);}v->as.instance=NULL;break;
        case VAL_TRAIT_DEF: {
            if (v->as.trait_def) {
                free(v->as.trait_def->name);
                for(int i=0; i<v->as.trait_def->method_count; i++) free(v->as.trait_def->method_names[i]);
                free(v->as.trait_def->method_names);
                free(v->as.trait_def->method_arity);
                free(v->as.trait_def);
            }
            v->as.trait_def = NULL;
            break;
        }
        default:break;
    }
    v->type=VAL_NONE;
}

/* ═══ List Operations ═══ */

void riz_list_append(RizList* list, RizValue v) {
    if(!list)return;
    if(list->count>=list->capacity){int nc=list->capacity<RIZ_INITIAL_CAP?RIZ_INITIAL_CAP:list->capacity*2;list->items=RIZ_GROW_ARRAY(RizValue,list->items,list->capacity,nc);list->capacity=nc;}
    list->items[list->count++]=v;
}
RizValue riz_list_get(RizList* list, int index) {
    if(!list||index<0||index>=list->count){riz_runtime_error("List index %d out of range (length %d)",index,list?list->count:0);return riz_none();}
    return riz_value_copy(list->items[index]);
}
int riz_list_length(RizList* list){return list?list->count:0;}

/* ═══ Dict Operations (Hash Table Index) ═══ */

static uint32_t hash_string(const char* key) {
    uint32_t hash = 2166136261u;
    for (int i = 0; key[i] != '\0'; i++) { hash ^= (uint8_t)key[i]; hash *= 16777619; }
    return hash;
}

static void dict_rehash(RizDict* d, int new_cap) {
    int* new_table = (int*)malloc(sizeof(int) * new_cap);
    for (int i = 0; i < new_cap; i++) new_table[i] = -1;
    for (int i = 0; i < d->count; i++) {
        uint32_t hash = hash_string(d->keys[i]);
        uint32_t index = hash & (new_cap - 1);
        while (new_table[index] != -1) index = (index + 1) & (new_cap - 1);
        new_table[index] = i;
    }
    free(d->hash_table);
    d->hash_table = new_table;
    d->hash_capacity = new_cap;
}

static int dict_find(RizDict* d, const char* key) {
    if (d->count == 0 || !d->hash_table) return -1;
    uint32_t hash = hash_string(key);
    uint32_t index = hash & (d->hash_capacity - 1);
    while (1) {
        int i = d->hash_table[index];
        if (i == -1) return -1;
        if (strcmp(d->keys[i], key) == 0) return i;
        index = (index + 1) & (d->hash_capacity - 1);
    }
}

void riz_dict_set(RizDict* d, const char* key, RizValue value) {
    int idx = dict_find(d, key);
    if (idx >= 0) { d->values[idx] = value; return; }
    if (d->count + 1 > d->hash_capacity * 0.75) dict_rehash(d, d->hash_capacity * 2);
    if (d->count >= d->capacity) {
        int nc = d->capacity < RIZ_INITIAL_CAP ? RIZ_INITIAL_CAP : d->capacity * 2;
        d->keys = RIZ_GROW_ARRAY(char*, d->keys, d->capacity, nc);
        d->values = RIZ_GROW_ARRAY(RizValue, d->values, d->capacity, nc);
        d->capacity = nc;
    }
    d->keys[d->count] = riz_strdup(key);
    d->values[d->count] = value;
    uint32_t hash = hash_string(key);
    uint32_t index = hash & (d->hash_capacity - 1);
    while (d->hash_table[index] != -1) index = (index + 1) & (d->hash_capacity - 1);
    d->hash_table[index] = d->count;
    d->count++;
}

RizValue riz_dict_get(RizDict* d, const char* key) {
    int idx = dict_find(d, key);
    if (idx < 0) return riz_none();
    return riz_value_copy(d->values[idx]);
}

bool riz_dict_has(RizDict* d, const char* key) { return dict_find(d, key) >= 0; }

void riz_dict_delete(RizDict* d, const char* key) {
    int idx = dict_find(d, key);
    if (idx < 0) return;
    free(d->keys[idx]);
    riz_value_free(&d->values[idx]);
    /* Shift array to keep it dense contiguous */
    for (int i = idx; i < d->count - 1; i++) {
        d->keys[i] = d->keys[i+1];
        d->values[i] = d->values[i+1];
    }
    d->count--;
    /* The indices are now offset. Fastest is to rehash since deletes are rare */
    dict_rehash(d, d->hash_capacity);
}

RizValue riz_dict_keys(RizDict* d) {
    RizValue list = riz_list_new();
    for(int i=0; i<d->count; i++) riz_list_append(list.as.list, riz_string(d->keys[i]));
    return list;
}

RizValue riz_dict_values(RizDict* d) {
    RizValue list = riz_list_new();
    for(int i=0; i<d->count; i++) riz_list_append(list.as.list, riz_value_copy(d->values[i]));
    return list;
}

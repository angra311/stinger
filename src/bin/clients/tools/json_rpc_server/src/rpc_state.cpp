#include <pthread.h>

#include "stinger_core/xmalloc.h"
#include "stinger_core/x86_full_empty.h"
#include "stinger_core/stinger_shared.h"
#include "rpc_state.h"
#include "stinger_core/stinger.h"

using namespace gt::stinger;


JSON_RPCServerState &
JSON_RPCServerState::get_server_state() {
  static JSON_RPCServerState state;
  return state;
}

JSON_RPCServerState::JSON_RPCServerState() : stinger(NULL), 
  stinger_loc(""), stinger_sz(0), algs(NULL), alg_map(NULL)
{
  pthread_rwlock_init(&alg_lock, NULL);
}

size_t
JSON_RPCServerState::get_num_algs()
{
  if(algs)
    return algs->size();
  else
    return 0;
}

StingerAlgState *
JSON_RPCServerState::get_alg(size_t num)
{
  if(algs)
    return (*algs)[num];
  else
    return NULL;
}

StingerAlgState *
JSON_RPCServerState::get_alg(const std::string & name)
{
  if(alg_map)
    return (*alg_map)[name];
  else
    return NULL;
}

bool
JSON_RPCServerState::has_alg(const std::string & name)
{
  if(alg_map)
    return alg_map->count(name) > 0;
  else
    return false;
}

void
JSON_RPCServerState::add_rpc_function(std::string name, JSON_RPCFunction * func)
{
  LOG_D_A("Adding RPC function %s", name.c_str());
  if(func)
    function_map[name] = func;
}

JSON_RPCFunction *
JSON_RPCServerState::get_rpc_function(std::string name)
{
  return function_map[name];
}

bool
JSON_RPCServerState::has_rpc_function(std::string name)
{
  return function_map.count(name) > 0;
}

void
JSON_RPCServerState::get_alg_read_lock()
{
  pthread_rwlock_rdlock(&alg_lock);
}

void
JSON_RPCServerState::release_alg_read_lock()
{
  pthread_rwlock_unlock(&alg_lock);
}

void
JSON_RPCServerState::update_algs(stinger_t * stinger_copy, std::string new_loc, int64_t new_sz, 
  std::vector<StingerAlgState *> * new_algs, std::map<std::string, StingerAlgState *> * new_alg_map)
{
  LOG_D_A("Called with %s, %ld", new_loc.c_str(), (long)new_sz);
  pthread_rwlock_wrlock(&alg_lock);
  /* remap stinger */
  if(stinger) {
    stinger_shared_free(stinger, stinger_loc.c_str(), stinger_sz);
  }
  stinger = stinger_copy;
  stinger_loc = new_loc;
  stinger_sz = new_sz;

  /* unmap / delete existing algs */
  if(algs) {
    for(int64_t i = 0; i < algs->size(); i++) {
      StingerAlgState * cur_alg = (*algs)[i];
      if(cur_alg) {
	shmunmap(cur_alg->data_loc.c_str(), cur_alg->data, cur_alg->data_per_vertex * STINGER_MAX_LVERTICES);
	delete cur_alg;
      }
    }

    delete algs;
  }
  if(alg_map)
    delete alg_map;

  algs = new_algs;
  alg_map = new_alg_map;

  LOG_D("About to unlock");
  pthread_rwlock_unlock(&alg_lock);
  LOG_D("Unlocked.");
}

bool
JSON_RPCFunction::contains_params(rpc_params_t * p, rapidjson::Value & params)
{
  while(p->name) {
    if(!params.HasMember(p->name)) {
      if(p->optional) {
	*((int64_t *)p->output) = p->def;
      } else {
	return false;
      }
    }
    else {

      switch(p->type) {
	case TYPE_INT64: {
	  if(!params[p->name].IsInt64()) {
	    return false;
	  }
	  *((int64_t *)p->output) = params[p->name].GetInt64();
	} break;
	case TYPE_STRING: {
	  if(!params[p->name].IsString()) {
	    return false;
	  }
	  *((char **)p->output) = (char *) params[p->name].GetString();
	} break;
	case TYPE_DOUBLE: {
	  if(!params[p->name].IsDouble()) {
	    return false;
	  }
	  *((double *)p->output) = params[p->name].GetDouble();
	} break;
	case TYPE_BOOL: {
	  if(!params[p->name].IsBool()) {
	    return false;
	  }
	  *((bool *)p->output) = params[p->name].GetBool();
	} break;
	case TYPE_ARRAY: {
	  if(!params[p->name].IsArray()) {
	    return false;
	  }
	  params_array_t * ptr = (params_array_t *) p->output;
	  ptr->len = params[p->name].Size();
	  ptr->arr = (int64_t *) xmalloc(sizeof(int64_t) * ptr->len);
	  stinger_t * S = server_state->get_stinger();
	  for (int64_t i = 0; i < ptr->len; i++) {
	    if (params[p->name][i].IsInt64()) {
	      ptr->arr[i] = params[p->name][i].GetInt64();
	    }
	    else if (params[p->name][i].IsString()) {
	      ptr->arr[i] = stinger_mapping_lookup(S, params[p->name][i].GetString(), params[p->name][i].GetStringLength());
	    }
	  }
	} break;
      }
    }
    p++;
  }
  return true;
}

stinger_t *
JSON_RPCServerState::get_stinger()
{
  return stinger;
}

params_array_t::params_array_t():arr(NULL) {}

params_array_t::~params_array_t() {
  if (arr)
    free (arr);
}

#include <cstdlib>
#include <cstdio>
#include <set>

//#define LOG_AT_W  /* warning only */

extern "C" {
#include "stinger_core/xmalloc.h"
#include "stinger_core/stinger_error.h"
}

#include "rapidjson/document.h"

#include "session_handling.h"
#include "json_rpc.h"
#include "alg_data_array.h"

using namespace gt::stinger;


rpc_params_t *
JSON_RPC_community_subgraph::get_params()
{
  return p;
}


int64_t
JSON_RPC_community_subgraph::update(const StingerBatch & batch)
{
  if (0 == batch.insertions_size () && 0 == batch.deletions_size ()) { 
    return 0;
  }

  stinger_t * S = server_state->get_stinger();
  if (!S) {
    LOG_E ("STINGER pointer is invalid");
    return -1;
  }

  for(size_t d = 0; d < batch.deletions_size(); d++) {
    const EdgeDeletion & del = batch.deletions(d);

    int64_t src = del.source();
    int64_t dst = del.destination();

    /* both source and destination vertices were previously tracked,
       i.e. the edge that was deleted was previously on the screen */
    if ( _vertices.find(src) != _vertices.end() && _vertices.find(dst) != _vertices.end() ) {
      _deletions.insert(std::make_pair(src, dst));
    }
  }

  /* edge insertions whose endpoints are within the same community get sent
     to the client on the next request */
  for (size_t i = 0; i < batch.insertions_size(); i++) {
    const EdgeInsertion & in = batch.insertions(i);
    
    int64_t src = in.source();
    int64_t dst = in.destination();

    LOG_D_A("src: %ld, dst: %ld, size: %ld, i: %ld", (long) src, (long) dst, (long) batch.insertions_size(), (long) i);
    if (_data->equal(src,dst)) {
      _insertions.insert(std::make_pair(src, dst));
    }
  }

  std::set<int64_t>::iterator it;

  for (it = _vertices.begin(); it != _vertices.end(); ++it) {
    if (!(_data->equal(_source, *it))) {

      STINGER_FORALL_EDGES_OF_VTX_BEGIN(S, *it) {
	/* edge used to be in the community */
	if ( _vertices.find(STINGER_EDGE_DEST) != _vertices.end() ) {
	  _deletions.insert(std::make_pair(*it, STINGER_EDGE_DEST));
	}
      } STINGER_FORALL_EDGES_OF_VTX_END();

      _vertices.erase(*it);  /* remove it from the vertex set */

    }
  }

  /* Add all vertices with the same label to the vertices[] set */
  LOG_D_A("_data->length() = %ld", (long) _data->length());
  for (int64_t i = 0; i < _data->length(); i++) {
    /* _source and i must be in the same community, and i must not be in the _vertices set */
    if (_data->equal(_source, i) && _vertices.find(i) == _vertices.end()) {
      _vertices.insert(i);

      STINGER_FORALL_EDGES_OF_VTX_BEGIN(S, i) {
	/* if the edge is in the community */
	if (_data->equal(i, STINGER_EDGE_DEST)) {
	  _insertions.insert(std::make_pair(i, STINGER_EDGE_DEST));
	}
      } STINGER_FORALL_EDGES_OF_VTX_END();
    }
  }

  return 0;
}


int64_t
JSON_RPC_community_subgraph::onRequest(
		      rapidjson::Value & result,
		      rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator> & allocator)
{
  rapidjson::Value insertions, deletions;
  rapidjson::Value src, dst, edge;
  std::set<std::pair<int64_t, int64_t> >::iterator it;

  /* send insertions back */
  insertions.SetArray();

  for (it = _insertions.begin(); it != _insertions.end(); ++it) {
    src.SetInt64((*it).first);
    dst.SetInt64((*it).second);
    edge.SetArray();
    edge.PushBack(src, allocator);
    edge.PushBack(dst, allocator);
    insertions.PushBack(edge, allocator);
  }

  result.AddMember("insertions", insertions, allocator);

  /* send deletions back */

  deletions.SetArray();

  for (it = _deletions.begin(); it != _deletions.end(); ++it) {
    src.SetInt64((*it).first);
    dst.SetInt64((*it).second);
    edge.SetArray();
    edge.PushBack(src, allocator);
    edge.PushBack(dst, allocator);
    deletions.PushBack(edge, allocator);
  }

  result.AddMember("deletions", deletions, allocator);

  /* clear both and reset the clock */
  _insertions.clear();
  _deletions.clear();
  reset_timeout();

  return 0;
}


int64_t
JSON_RPC_community_subgraph::onRegister(
		      rapidjson::Value & result,
		      rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator> & allocator)
{
  stinger_t * S = server_state->get_stinger();
  if (!S) {
    LOG_E ("STINGER pointer is invalid");
    return json_rpc_error(-32603, result, allocator);
  }

  StingerAlgState * alg_state = server_state->get_alg(_algorithm_name);
  if (!alg_state) {
    LOG_E ("Algorithm is not running");
    return json_rpc_error(-32603, result, allocator);
  }

  const char * description_string = alg_state -> data_description.c_str();
  int64_t nv = STINGER_MAX_LVERTICES;
  uint8_t * data = (uint8_t *) alg_state->data;
  const char * search_string = _data_array_name;

  _data = description_string_to_pointer (alg_state, description_string, data, nv, search_string);
  AlgDataArray * df = _data;

  if (df->type() != 'l') {
    return json_rpc_error(-32602, result, allocator);
  }

  /* Get the community label of the source vertex */
  int64_t community_id = df->get_int64(_source);

  /* Add all vertices with the same label to the vertices[] set */
  for (int64_t i = 0; i < df->length(); i++) {
    if (df->equal(_source, i)) {
      _vertices.insert(i);
    }
  }

  rapidjson::Value a (rapidjson::kArrayType);
  rapidjson::Value src, dst, edge;

  /* Get all edges within the community */
  std::set<int64_t>::iterator it;
  for (it = _vertices.begin(); it != _vertices.end(); ++it) {
    STINGER_FORALL_EDGES_OF_VTX_BEGIN (S, *it) {
      if (_vertices.find(STINGER_EDGE_DEST) != _vertices.end()) {
	// edge is in the community
	src.SetInt64(*it);
	dst.SetInt64(STINGER_EDGE_DEST);
	edge.SetArray();
	edge.PushBack(src, allocator);
	edge.PushBack(dst, allocator);
	a.PushBack(edge, allocator);
      }
    } STINGER_FORALL_EDGES_OF_VTX_END();
  }

  result.AddMember("subgraph", a, allocator);

  return 0;
}


AlgDataArray *
description_string_to_pointer (gt::stinger::StingerAlgState * alg_state, const char * description_string,
				uint8_t * data,
				int64_t nv,
				const char * search_string)
{
  size_t off = 0;
  size_t len = strlen(description_string);
  char * tmp = (char *) xmalloc ((len+1) * sizeof(char));
  strcpy(tmp, description_string);

  /* the description string is space-delimited */
  char * ptr = strtok (tmp, " ");

  /* skip the formatting */
  char * pch = strtok (NULL, " ");

  while (pch != NULL)
  {
    if (strcmp(pch, search_string) == 0) {
      AlgDataArray * rtn = new AlgDataArray(alg_state, data, description_string[off], nv);
      free (tmp);
      return rtn;

    } else {
      switch (description_string[off]) {
	case 'f':
	  data += (nv * sizeof(float));
	  break;

	case 'd':
	  data += (nv * sizeof(double));
	  break;

	case 'i':
	  data += (nv * sizeof(int32_t));
	  break;

	case 'l':
	  data += (nv * sizeof(int64_t));
	  break;

	case 'b':
	  data += (nv * sizeof(uint8_t));
	  break;

	default:
	  LOG_W_A("Umm...what letter was that?\ndescription_string: %s", description_string);
	  //return json_rpc_error(-32603, rtn, allocator);
	  return NULL;

      }
      off++;
    }

    pch = strtok (NULL, " ");
  }

  free(tmp);
  return NULL;
}

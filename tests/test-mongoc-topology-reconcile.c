#include <mongoc.h>
#include <mongoc-util-private.h>

#include "mongoc-client-pool-private.h"
#include "mongoc-client-private.h"
#include "utlist.h"

#include "mock_server/future.h"
#include "mock_server/future-functions.h"
#include "mock_server/mock-server.h"
#include "TestSuite.h"
#include "test-conveniences.h"
#include "test-libmongoc.h"

#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "topology-reconcile-test"


static mongoc_topology_scanner_node_t *
get_node (mongoc_topology_t *topology, const char *host_and_port)
{
   mongoc_topology_scanner_t *ts;
   mongoc_topology_scanner_node_t *node;
   mongoc_topology_scanner_node_t *sought = NULL;

   mongoc_mutex_lock (&topology->mutex);

   ts = topology->scanner;

   DL_FOREACH (ts->nodes, node)
   {
      if (!strcmp (host_and_port, node->host.host_and_port)) {
         sought = node;
         break;
      }
   }

   mongoc_mutex_unlock (&topology->mutex);

   return sought;
}


void
rs_response_to_ismaster (mock_server_t *server, bool primary, int has_tags, ...)
{
   va_list ap;
   bson_string_t *hosts;
   bool first;
   mock_server_t *host;
   char *ismaster_response;

   hosts = bson_string_new ("");

   va_start (ap, has_tags);

   first = true;
   while ((host = va_arg (ap, mock_server_t *))) {
      if (first) {
         first = false;
      } else {
         bson_string_append (hosts, ",");
      }

      bson_string_append_printf (
         hosts, "'%s'", mock_server_get_host_and_port (host));
   }

   va_end (ap);

   ismaster_response = bson_strdup_printf ("{'ok': 1, "
                                           " 'setName': 'rs',"
                                           " 'ismaster': %s,"
                                           " 'secondary': %s,"
                                           " 'tags': {%s},"
                                           " 'hosts': [%s]"
                                           "}",
                                           primary ? "true" : "false",
                                           primary ? "false" : "true",
                                           has_tags ? "'key': 'value'" : "",
                                           hosts->str);

   mock_server_auto_ismaster (server, ismaster_response);

   bson_free (ismaster_response);
   bson_string_free (hosts, true);
}


#define RS_RESPONSE_TO_ISMASTER(server, primary, has_tags, ...) \
   rs_response_to_ismaster (server, primary, has_tags, __VA_ARGS__, NULL)


bool
selects_server (mongoc_client_t *client,
                mongoc_read_prefs_t *read_prefs,
                mock_server_t *server)
{
   bson_error_t error;
   mongoc_server_description_t *sd;
   bool result;

   sd = mongoc_topology_select (
      client->topology, MONGOC_SS_READ, read_prefs, &error);

   if (!sd) {
      fprintf (stderr, "%s\n", error.message);
      return false;
   }

   result = (0 == strcmp (mongoc_server_description_host (sd)->host_and_port,
                          mock_server_get_host_and_port (server)));

   mongoc_server_description_destroy (sd);

   return result;
}


static void
_test_topology_reconcile_rs (bool pooled)
{
   mock_server_t *server0;
   mock_server_t *server1;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool = NULL;
   mongoc_client_t *client;
   debug_stream_stats_t debug_stream_stats = {0};
   mongoc_read_prefs_t *secondary_read_prefs;
   mongoc_read_prefs_t *primary_read_prefs;
   mongoc_read_prefs_t *tag_read_prefs;

   server0 = mock_server_new ();
   server1 = mock_server_new ();
   mock_server_run (server0);
   mock_server_run (server1);

   /* secondary, no tags */
   RS_RESPONSE_TO_ISMASTER (server0, false, false, server0, server1);
   /* primary, no tags */
   RS_RESPONSE_TO_ISMASTER (server1, true, false, server0, server1);

   /* provide secondary in seed list */
   uri_str = bson_strdup_printf ("mongodb://%s/?replicaSet=rs",
                                 mock_server_get_host_and_port (server0));

   uri = mongoc_uri_new (uri_str);

   if (pooled) {
      pool = mongoc_client_pool_new (uri);
      client = mongoc_client_pool_pop (pool);
   } else {
      client = mongoc_client_new (uri_str);
   }

   if (!pooled) {
      test_framework_set_debug_stream (client, &debug_stream_stats);
   }

   secondary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_SECONDARY);
   primary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);
   tag_read_prefs = mongoc_read_prefs_new (MONGOC_READ_NEAREST);
   mongoc_read_prefs_add_tag (tag_read_prefs, tmp_bson ("{'key': 'value'}"));

   /*
    * server0 is selected, server1 is discovered and added to scanner.
    */
   assert (selects_server (client, secondary_read_prefs, server0));
   assert (
      get_node (client->topology, mock_server_get_host_and_port (server1)));

   /*
    * select again with mode "primary": server1 is selected.
    */
   assert (selects_server (client, primary_read_prefs, server1));

   /*
    * remove server1 from set. server0 is the primary, with tags.
    */
   RS_RESPONSE_TO_ISMASTER (server0, true, true, server0); /* server1 absent */

   assert (selects_server (client, tag_read_prefs, server0));
   assert (!client->topology->stale);

   if (!pooled) {
      ASSERT_CMPINT (1, ==, debug_stream_stats.n_failed);
   }

   /*
    * server1 returns as a secondary. its scanner node is un-retired.
    */
   RS_RESPONSE_TO_ISMASTER (server0, true, true, server0, server1);
   RS_RESPONSE_TO_ISMASTER (server1, false, false, server0, server1);

   assert (selects_server (client, secondary_read_prefs, server1));

   if (!pooled) {
      /* no additional failed streams */
      ASSERT_CMPINT (1, ==, debug_stream_stats.n_failed);
   }

   mongoc_read_prefs_destroy (primary_read_prefs);
   mongoc_read_prefs_destroy (secondary_read_prefs);
   mongoc_read_prefs_destroy (tag_read_prefs);

   if (pooled) {
      mongoc_client_pool_push (pool, client);
      mongoc_client_pool_destroy (pool);
   } else {
      mongoc_client_destroy (client);
   }

   mongoc_uri_destroy (uri);
   bson_free (uri_str);
   mock_server_destroy (server1);
   mock_server_destroy (server0);
}


static void
test_topology_reconcile_rs_single (void *ctx)
{
   _test_topology_reconcile_rs (false);
}


static void
test_topology_reconcile_rs_pooled (void *ctx)
{
   _test_topology_reconcile_rs (true);
}


static void
_test_topology_reconcile_sharded (bool pooled)
{
   mock_server_t *mongos;
   mock_server_t *secondary;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool = NULL;
   mongoc_client_t *client;
   mongoc_read_prefs_t *primary_read_prefs;
   bson_error_t error;
   future_t *future;
   request_t *request;
   char *secondary_response;
   mongoc_server_description_t *sd;

   mongos = mock_server_new ();
   secondary = mock_server_new ();
   mock_server_run (mongos);
   mock_server_run (secondary);

   /* provide both servers in seed list */
   uri_str = bson_strdup_printf ("mongodb://%s,%s",
                                 mock_server_get_host_and_port (mongos),
                                 mock_server_get_host_and_port (secondary));

   uri = mongoc_uri_new (uri_str);

   if (pooled) {
      pool = mongoc_client_pool_new (uri);
      client = mongoc_client_pool_pop (pool);
   } else {
      client = mongoc_client_new (uri_str);
   }

   primary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);
   future = future_topology_select (
      client->topology, MONGOC_SS_READ, primary_read_prefs, &error);

   /* mongos */
   request = mock_server_receives_ismaster (mongos);
   mock_server_replies_simple (
      request, "{'ok': 1, 'ismaster': true, 'msg': 'isdbgrid'}");

   request_destroy (request);

   /* make sure the mongos response is processed first */
   _mongoc_usleep (1000 * 1000);

   /* replica set secondary - topology removes it */
   request = mock_server_receives_ismaster (secondary);
   secondary_response =
      bson_strdup_printf ("{'ok': 1, "
                          " 'setName': 'rs',"
                          " 'ismaster': false,"
                          " 'secondary': true,"
                          " 'hosts': ['%s', '%s']"
                          "}",
                          mock_server_get_host_and_port (mongos),
                          mock_server_get_host_and_port (secondary));

   mock_server_replies_simple (request, secondary_response);

   request_destroy (request);

   /*
    * mongos is selected, secondary is removed.
    */
   sd = future_get_mongoc_server_description_ptr (future);
   ASSERT_CMPSTR (sd->host.host_and_port,
                  mock_server_get_host_and_port (mongos));

   if (pooled) {
      /* wait a second for scanner thread to remove secondary */
      int64_t start = bson_get_monotonic_time ();
      while (get_node (client->topology,
                       mock_server_get_host_and_port (secondary))) {
         ASSERT_CMPTIME ((int) (bson_get_monotonic_time () - start),
                         (int) 1000000);
      }
   } else {
      assert (!get_node (client->topology,
                         mock_server_get_host_and_port (secondary)));
   }

   mongoc_server_description_destroy (sd);
   bson_free (secondary_response);
   mongoc_read_prefs_destroy (primary_read_prefs);

   if (pooled) {
      mongoc_client_pool_push (pool, client);
      mongoc_client_pool_destroy (pool);
   } else {
      mongoc_client_destroy (client);
   }

   future_destroy (future);
   mongoc_uri_destroy (uri);
   bson_free (uri_str);
   mock_server_destroy (secondary);
   mock_server_destroy (mongos);
}


static void
test_topology_reconcile_sharded_single (void)
{
   _test_topology_reconcile_sharded (false);
}


static void
test_topology_reconcile_sharded_pooled (void)
{
   _test_topology_reconcile_sharded (true);
}


/* CDRIVER-2552 in mongoc_topology_scanner_node_setup, assert (!node->retired)
 * failed after this sequence in pooled mode:
 *
 * 1. scanner discovers a replica set with primary and at least one secondary
 * 2. cluster opens a new stream to the primary
 * 3. cluster handshakes the new connection by calling isMaster on the primary
 * 4. the primary, for some reason, suddenly omits the secondary from its host
 *    list, perhaps because the secondary was removed from the RS configuration
 * 5. scanner marks the secondary scanner node "retired" to be destroyed later
 * 6. the scanner is disconnected from the secondary for some reason
 * 7. on the next scan, mongoc_topology_scanner_node_setup sees that the
 *    secondary is disconnected, and before creating a new stream it asserts
 *    !node->retired.
 *
 * test that between step 5 and 7, mongoc_topology_scanner_reset destroys the
 * secondary node, avoiding the assert failure. test both pooled and single
 * mode for good measure.
 */
static void
_test_topology_reconcile_retire (bool pooled)
{
   mock_server_t *secondary;
   mock_server_t *primary;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool = NULL;
   mongoc_client_t *client;
   mongoc_topology_t *topology;
   mongoc_read_prefs_t *primary_read_prefs;
   mongoc_read_prefs_t *secondary_read_prefs;
   mongoc_read_prefs_t *tag_read_prefs;
   mongoc_topology_scanner_node_t *node;
   bson_error_t error;
   future_t *future;
   request_t *request;

   secondary = mock_server_new ();
   primary = mock_server_new ();
   mock_server_run (secondary);
   mock_server_run (primary);

   RS_RESPONSE_TO_ISMASTER (primary, true, false, secondary, primary);
   RS_RESPONSE_TO_ISMASTER (secondary, false, false, secondary, primary);

   /* selection timeout must be > MONGOC_TOPOLOGY_MIN_HEARTBEAT_FREQUENCY_MS,
    * otherwise we skip second scan in pooled mode and don't hit the assert */
   uri_str = bson_strdup_printf (
      "mongodb://%s,%s/?replicaSet=rs"
      "&serverSelectionTimeoutMS=600&heartbeatFrequencyMS=999999999",
      mock_server_get_host_and_port (primary),
      mock_server_get_host_and_port (secondary));

   uri = mongoc_uri_new (uri_str);

   if (pooled) {
      pool = mongoc_client_pool_new (uri);
      topology = _mongoc_client_pool_get_topology (pool);
      client = mongoc_client_pool_pop (pool);
   } else {
      client = mongoc_client_new (uri_str);
      topology = client->topology;
   }

   /* step 1: discover both replica set members */
   primary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);
   BSON_ASSERT (selects_server (client, primary_read_prefs, primary));
   secondary_read_prefs = mongoc_read_prefs_new (MONGOC_READ_SECONDARY);
   BSON_ASSERT (selects_server (client, secondary_read_prefs, secondary));

   /* remove secondary from primary's config */
   mongoc_mutex_lock (&topology->mutex);
   RS_RESPONSE_TO_ISMASTER (primary, true, false, primary);

   /* step 2: cluster opens new stream to primary - force new stream in single
    * mode by disconnecting scanner nodes (also includes step 6) */
   DL_FOREACH (topology->scanner->nodes, node)
   {
      BSON_ASSERT (node);
      BSON_ASSERT (node->stream);
      mongoc_stream_destroy (node->stream);
      node->stream = NULL;
   }
   mongoc_mutex_unlock (&topology->mutex);

   /* step 3: run "ping" on primary, triggering a connection and handshake, thus
    * step 4 & 5: the primary tells the scanner to retire the secondary node */
   future = future_client_read_command_with_opts (
      client, "admin", tmp_bson ("{'ping': 1}"), NULL, NULL, NULL, &error);
   request = mock_server_receives_command (
      primary, "admin", MONGOC_QUERY_NONE, "{'ping': 1}");
   mock_server_replies_ok_and_destroys (request);
   ASSERT_OR_PRINT (future_get_bool (future), error);

   node = get_node (topology, mock_server_get_host_and_port (secondary));
   BSON_ASSERT (node);
   BSON_ASSERT (node->retired);

   /* step 7: trigger a scan by selecting with an unsatisfiable read preference.
    * should not crash with BSON_ASSERT. */
   tag_read_prefs = mongoc_read_prefs_new (MONGOC_READ_NEAREST);
   mongoc_read_prefs_add_tag (tag_read_prefs, tmp_bson ("{'key': 'value'}"));
   BSON_ASSERT (
      !mongoc_client_select_server (client, false, tag_read_prefs, NULL));

   if (pooled) {
      mongoc_client_pool_push (pool, client);
      mongoc_client_pool_destroy (pool);
   } else {
      mongoc_client_destroy (client);
   }

   future_destroy (future);
   mock_server_destroy (primary);
   mock_server_destroy (secondary);
   mongoc_read_prefs_destroy (primary_read_prefs);
   mongoc_read_prefs_destroy (secondary_read_prefs);
   mongoc_read_prefs_destroy (tag_read_prefs);
   mongoc_uri_destroy (uri);
   bson_free (uri_str);
}


static void
test_topology_reconcile_retire_single (void *ctx)
{
   _test_topology_reconcile_retire (false);
}


static void
test_topology_reconcile_retire_pooled (void *ctx)
{
   _test_topology_reconcile_retire (true);
}


void
test_topology_reconcile_install (TestSuite *suite)
{
   TestSuite_AddFull (suite,
                      "/TOPOLOGY/reconcile/rs/pooled",
                      test_topology_reconcile_rs_pooled,
                      NULL,
                      NULL,
                      test_framework_skip_if_slow);
   TestSuite_AddFull (suite,
                      "/TOPOLOGY/reconcile/rs/single",
                      test_topology_reconcile_rs_single,
                      NULL,
                      NULL,
                      test_framework_skip_if_slow);
   TestSuite_Add (suite,
                  "/TOPOLOGY/reconcile/sharded/pooled",
                  test_topology_reconcile_sharded_pooled);
   TestSuite_Add (suite,
                  "/TOPOLOGY/reconcile/sharded/single",
                  test_topology_reconcile_sharded_single);
   TestSuite_AddFull (suite,
                      "/TOPOLOGY/reconcile/retire/pooled",
                      test_topology_reconcile_retire_pooled,
                      NULL,
                      NULL,
                      test_framework_skip_if_slow);
   TestSuite_AddFull (suite,
                      "/TOPOLOGY/reconcile/retire/single",
                      test_topology_reconcile_retire_single,
                      NULL,
                      NULL,
                      test_framework_skip_if_slow);
}

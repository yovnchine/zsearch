/*
  A trivial static http webserver using Libevent's evhttp.

  This is not the best code in the world, and it does some fairly stupid stuff
  that you would never want to do in a production webserver. Caveat hackor!

 */


#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifndef S_ISDIR
#define S_ISDIR(x) (((x) & S_IFMT) == S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#ifdef EVENT__HAVE_NETINET_IN_H
#include <netinet/in.h>
# ifdef _XOPEN_SOURCE_EXTENDED
#  include <arpa/inet.h>
# endif
#endif

#ifdef _WIN32
#define stat _stat
#define fstat _fstat
#define open _open
#define close _close
#define O_RDONLY _O_RDONLY
#endif

#include <string>
#include <memory>
#include <exception>
#include <iostream>
#include <chrono>

#include "DocumentStoreSimple.h"
#include "TokenizerImpl.h"
#include "DocumentImpl.hpp"
#include "KVStoreLevelDb.h"
#include "Constants.hpp"
#include "Engine.hpp"
#include "varint/BasicSet.h"
#include "varint/SetFactory.h"
#include "varint/BasicSetFactory.h"

static const std::string POST_HTM = "/post.htm";
static const std::string SEARCH_PATH = "/search";
static const std::string POST_PATH = "/post";
static const std::string DOC_PATH = "/doc";
static const std::string INDEX_PATH = "/index";
static const std::string ROOT = "/";

static Engine *engine;

char uri_root[512];

static unsigned int getDocIdFromString(const std::string& strDocId)
{
	unsigned int docId;
	stringstream ss(strDocId);
	ss >> docId;
	return docId;
}

static void printTimeTaken(const std::chrono::nanoseconds& ns)
{
	if (ns.count() >= 1000000000)
	{
		std::cout << "function returned in " << std::chrono::duration_cast<std::chrono::seconds>(ns).count() << "s" << std::endl;
	}
	else if (ns.count() >= 1000000)
	{
		std::cout << "function returned in " << std::chrono::duration_cast<std::chrono::milliseconds>(ns).count() << "ms" << std::endl;
	}
	else
	{
		std::cout << "function returned in " << ns.count() << "ns" << std::endl;
	}
}

/**
 * Callback used for doc request
 */
static void doc_request_cb(struct evhttp_request *req, void *arg)
{
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET)
	{
		std::cerr << "Invalid query request! Needs to be GET" << std::endl;
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
	}
	else
	{
		struct evbuffer *evb = NULL;
		const char *uri = evhttp_request_get_uri(req);
		struct evhttp_uri *decoded = NULL;
		const char *path = NULL;
		const char *query = NULL;

		printf("Got a GET request for %s\n",  uri);

		// Decode the URI
		decoded = evhttp_uri_parse(uri);

		if (!decoded)
		{
			printf("It's not a good URI. Sending BADREQUEST\n");
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
			return;
		}

		path = evhttp_uri_get_path(decoded);
		std::cout << path << std::endl;

		query = evhttp_uri_get_query(decoded);
		std::cout << query << std::endl;

		// This holds the content we're sending
		evb = evbuffer_new();

		struct evkeyvalq params;	// create storage for your key->value pairs
		struct evkeyval *param;		// iterator

		int result = evhttp_parse_query_str(query, &params);

		if (result == 0)
		{
			bool found = false;

			for (param = params.tqh_first; param; param = param->next.tqe_next)
			{
				std::string key(param->key);
				std::string value(param->value);

				printf("%s\n%s\n", key.c_str(), value.c_str());

				if (key.compare(zsearch::DOC_ID_KEY) == 0)
				{
					std::cout << "retrieving document " << value << std::endl;

					unsigned int docId = getDocIdFromString(value);

					std::shared_ptr<IDocument> document;

					if (engine->getDoc(docId, document))
					{
						std::stringstream ss;
						document->write(ss);
						const std::string docStr = ss.str();
						// cout << docStr << endl;

						evbuffer_add(evb, docStr.data(), docStr.size());
						found = true;
					}

					break;
				}
			}

			if (found)
			{
				evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/xml");
				evhttp_send_reply(req, 200, "OK", evb);
			}
			else
			{
				/*
				evbuffer_add_printf(evb, "Document not found or invalid docId");
				evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
				*/

				evhttp_send_error(req, 404, "Document not found.");
			}

		}
		else
		{
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
		}

		evhttp_clear_headers(&params);


		if (decoded)
		{
			evhttp_uri_free(decoded);
		}

		if (evb)
		{
			evbuffer_free(evb);
		}
	}

	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	std::chrono::nanoseconds timeTaken = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
	printTimeTaken(timeTaken);
}

/**
 * Callback used for search request
 */
static void search_request_cb(struct evhttp_request *req, void *arg)
{
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET)
	{
		// evbuffer_add_printf(evb, "Invalid query request! Needs to be GET\n");
		std::cerr << "Invalid query request! Needs to be GET" << std::endl;
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
	}
	else
	{
		struct evbuffer *evb = NULL;
		const char *uri = evhttp_request_get_uri(req);
		struct evhttp_uri *decoded = NULL;
		const char *path = NULL;
		const char *query = NULL;
		// struct evkeyvalq *headers;
		// struct evkeyval *header;

		printf("Got a GET request for %s\n",  uri);

		// Decode the URI
		decoded = evhttp_uri_parse(uri);
		if (!decoded) {
			printf("It's not a good URI. Sending BADREQUEST\n");
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
			return;
		}

		path = evhttp_uri_get_path(decoded);
		std::cout << path << std::endl;

		query = evhttp_uri_get_query(decoded);
		std::cout << query << std::endl;

		// This holds the content we're sending
		evb = evbuffer_new();

		/*
		headers = evhttp_request_get_input_headers(req);
		for (header = headers->tqh_first; header;
			header = header->next.tqe_next) {
			printf("  %s: %s\n", header->key, header->value);
		}
		*/

		struct evkeyvalq params;	// create storage for your key->value pairs
		struct evkeyval *param;		// iterator

		int result = evhttp_parse_query_str(query, &params);

		if (result == 0)
		{
			std::string query;
			unsigned int start = 0;
			unsigned int offset = 0;

			for (param = params.tqh_first; param; param = param->next.tqe_next)
			{
				std::string key(param->key);
				std::string value(param->value);

				std::cout << key << " " << value << std::endl;

				if (key.compare(zsearch::GET_SEARCH_QUERY_KEY) == 0)
				{
					query = value;
				}
			}

			std::cout << "searching for " << query << std::endl;

			auto docIdSet = engine->search(query, start, offset);

			if (docIdSet.size())
			{
				for (auto docId : docIdSet)
				{
					evbuffer_add_printf(evb, "%u ", docId);
				}

				/*
				auto docSet = engine->getDocs(docIdSet);

				for (auto document : docSet)
				{
					std::string title;
					document->getEntry("title", title);
					std::cout << title << " ";
					evbuffer_add_printf(evb, title.c_str());
					evbuffer_add_printf(evb, "\n");
				}
				*/

				// evbuffer_add_printf(evb, "\n");
			}

			evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
			evhttp_send_reply(req, 200, "OK", evb);

		}
		else
		{
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
		}

		evhttp_clear_headers(&params);


		if (decoded)
		{
			evhttp_uri_free(decoded);
		}

		if (evb)
		{
			evbuffer_free(evb);
		}
	}

	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	std::chrono::nanoseconds timeTaken = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
	printTimeTaken(timeTaken);
}

/**
 * Call back used for a POST request
 */
static void post_request_cb(struct evhttp_request *req, void *arg)
{
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();

	/*

	bool isPostRequest = false;
	const char *cmdtype = NULL;

	switch (evhttp_request_get_command(req))
	{
		case EVHTTP_REQ_GET: cmdtype = "GET"; break;
		case EVHTTP_REQ_POST: cmdtype = "POST"; isPostRequest = true; break;
		case EVHTTP_REQ_HEAD: cmdtype = "HEAD"; break;
		case EVHTTP_REQ_PUT: cmdtype = "PUT"; break;
		case EVHTTP_REQ_DELETE: cmdtype = "DELETE"; break;
		case EVHTTP_REQ_OPTIONS: cmdtype = "OPTIONS"; break;
		case EVHTTP_REQ_TRACE: cmdtype = "TRACE"; break;
		case EVHTTP_REQ_CONNECT: cmdtype = "CONNECT"; break;
		case EVHTTP_REQ_PATCH: cmdtype = "PATCH"; break;
		default: cmdtype = "unknown"; break;
	}

	if (!isPostRequest)
	{
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
	}
	*/

	if (evhttp_request_get_command(req) != EVHTTP_REQ_POST)
	{
		std::cerr << "Invalid request! Needs to be POST" << std::endl;
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
	}
	else
	{
		struct evbuffer *evb = NULL;
		struct evkeyvalq *headers;
		struct evkeyval *header;
		struct evbuffer *buf;

		printf("Received a POST request for %s\nHeaders:\n", evhttp_request_get_uri(req));

		headers = evhttp_request_get_input_headers(req);

		for (header = headers->tqh_first; header; header = header->next.tqe_next)
		{
			printf("  %s: %s\n", header->key, header->value);
		}

		buf = evhttp_request_get_input_buffer(req);

		std::string postData;

		while (evbuffer_get_length(buf))
		{
			int n;
			char cbuf[128];
			n = evbuffer_remove(buf, cbuf, sizeof(buf)-1);
			if (n > 0)
			{
				// (void) fwrite(cbuf, 1, n, stdout);
				postData.append(cbuf, n);
			}
		}

		// std::cout << "Post data: " << std::endl << postData << std::endl;

		// do not remove this
		struct evkeyvalq params;	// create storage for your key->value pairs
		struct evkeyval *param;		// iterator

		// working code to return the parameters as plain text ...
		/*
		std::string postDataDecoded;

		if (result == 0)
		{
			for (param = params.tqh_first; param; param = param->next.tqe_next)
			{
				printf("%s %s\n", param->key, param->value);
				postDataDecoded.append(param->key);
				postDataDecoded.append(" ");
				postDataDecoded.append(param->value);
				postDataDecoded.append("\n");
			}

			evb = evbuffer_new();
			evbuffer_add_printf(evb, postDataDecoded.c_str());
			evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");

			evhttp_send_reply(req, 200, "OK", evb);
		}
		else
		{
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
		}
		*/

		// working code to decode and index data

		int result = evhttp_parse_query_str(postData.c_str(), &params);

		// if we were able to parse post data ok
		if (result == 0)
		{
			param = params.tqh_first;

			std::string key(param->key);
			std::string value(param->value);

			// std::cout << value << std::endl;

			evb = evbuffer_new();

			// check that the first key is data
			if (key.compare(zsearch::POST_DATA_KEY) == 0)
			{
				try
				{
					std::shared_ptr<IDocument> document = std::make_shared<DocumentImpl>(value);
					unsigned int docId = engine->addDocument(document);
					std::cout << "Added document: " << docId << std::endl;
					evbuffer_add_printf(evb, "%d", docId);
				}
				catch (const std::string& e)
				{
					evbuffer_add_printf(evb, "Error parsing document. See documentation for more details\n");
					evbuffer_add_printf(evb, e.c_str());
				}
				catch (const std::exception& e)
				{
					evbuffer_add_printf(evb, "Error parsing document. See documentation for more details\n");
					evbuffer_add_printf(evb, e.what());
				}
			}
			else
			{
				evbuffer_add_printf(evb, "Invalid post data, first key must be in the form of data -> {xml}. See documentation for more details\n");
			}

			evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
			evhttp_send_reply(req, 200, "OK", evb);

		}
		else
		{
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
		}

		// this will segfault
		/*
		evb = evbuffer_new();

		std::string keyCompare = zsearch::POST_DATA_KEY + "=";

		if (keyCompare.compare(postData.substr(0, 5)) == 0)
		{
			// data=
			std::string value = postData.substr(5);
			std::cout << value << std::endl;

			try
			{
				std::shared_ptr<IDocument> document = std::make_shared<DocumentImpl>(value);
				std::cout << "made document" << std::endl;
				unsigned int docId = engine->addDocument(document);
				std::cout << "Added document: " << docId << std::endl;
				evbuffer_add_printf(evb, "%u", docId);
			}
			catch (const std::string& e)
			{
				evbuffer_add_printf(evb, "Error parsing document. See documentation for more details\n");
				evbuffer_add_printf(evb, e.c_str());
			}
			catch (const std::exception& e)
			{
				evbuffer_add_printf(evb, "Error parsing document. See documentation for more details\n");
				evbuffer_add_printf(evb, e.what());
			}
		}
		else
		{
			evbuffer_add_printf(evb, "Invalid post data, first key must be in the form of data -> {xml}. See documentation for more details\n");
		}

		evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
		evhttp_send_reply(req, 200, "OK", evb);
		*/

		evhttp_clear_headers(&params);

		if (evb)
		{
			evbuffer_free(evb);
		}
	}

	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	std::chrono::nanoseconds timeTaken = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
	printTimeTaken(timeTaken);
}

/**
 * This callback gets invoked when we get any http request that doesn't match
 * any other callback.  Like any evhttp server callback, it has a simple job:
 * it must eventually call evhttp_send_error() or evhttp_send_reply().
 */
static void generic_request_cb(struct evhttp_request *req, void *arg)
{
	std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();

	// if this is not a GET request error out
	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET)
	{
		std::cerr << "Invalid request! Needs to be GET" << std::endl;
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
	}
	else
	{
		struct evbuffer *evb = NULL;
		const char *docroot = (char *) arg;
		const char *uri = evhttp_request_get_uri(req);
		struct evhttp_uri *decoded = NULL;
		const char *path;
		char *decoded_path;
		char *whole_path = NULL;
		size_t len;
		int fd = -1;
		struct stat st;

		printf("Got a GET request for %s\n",  uri);

		// Decode the URI
		decoded = evhttp_uri_parse(uri);
		if (!decoded)
		{
			printf("It's not a good URI. Sending BADREQUEST\n");
			evhttp_send_error(req, HTTP_BADREQUEST, 0);
			return;
		}

		// Let's see what path the user asked for
		path = evhttp_uri_get_path(decoded);
		if (!path)
		{
			path = ROOT.c_str();
		}

		// We need to decode it, to see what path the user really wanted
		decoded_path = evhttp_uridecode(path, 0, NULL);

		bool error = false;

		if (decoded_path == NULL)
		{
			error = true;
		}
		else
		{
			// This holds the content we're sending
			evb = evbuffer_new();

			// add headers
			evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");

			if (POST_HTM.compare(decoded_path) == 0)
			{
				len = strlen(decoded_path)+strlen(docroot)+2;
				whole_path = (char *) malloc(len);

				if (whole_path)
				{
					evutil_snprintf(whole_path, len, "%s/%s", docroot, decoded_path);

					if (stat(whole_path, &st)<0)
					{
						error = true;
					}
					else
					{
						if ((fd = open(whole_path, O_RDONLY)) < 0)
						{
							perror("open");
							error = true;

						}
						else
						{
							if (fstat(fd, &st)<0)
							{
								// Make sure the length still matches, now that we opened the file :/
								perror("fstat");
								error = true;
							}
							else
							{
								evbuffer_add_file(evb, fd, 0, st.st_size);
							}
						}
					}
				}
				else
				{
					perror("malloc");
					error = true;
				}
			}
			else // if (ROOT.compare(decoded_path) == 0)
			{
				evbuffer_add_printf(evb, "Invalid request <br />\n");
				evbuffer_add_printf(evb, "%s to post data manually or %s to post via api<br />\n", POST_HTM.c_str(), INDEX_PATH.c_str());
				evbuffer_add_printf(evb, "%s to search <br />\n", SEARCH_PATH.c_str());
				evbuffer_add_printf(evb, "%s to get document by id <br />\n", DOC_PATH.c_str());
			}
		}

		if (error)
		{
			if (fd >= 0)
				close(fd);

			evhttp_send_error(req, 404, "Document not found.");
		}
		else
		{
			evhttp_send_reply(req, 200, "OK", evb);
		}

		if (decoded)
			evhttp_uri_free(decoded);
		if (decoded_path)
			free(decoded_path);
		if (whole_path)
			free(whole_path);
		if (evb)
			evbuffer_free(evb);
	}

	std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
	std::chrono::nanoseconds timeTaken = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
	printTimeTaken(timeTaken);
}


static void syntax(void)
{
	fprintf(stdout, "Syntax: http-server <docroot>\n");
}

int main(int argc, char **argv)
{
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *handle;

    std::shared_ptr<ISetFactory> setFactory = make_shared<BasicSetFactory>();
	std::shared_ptr<ITokenizer> tokenizer = std::make_shared<TokenizerImpl>(zsearch::QUERY_PARSER_DELIMITERS);
	std::shared_ptr<IDocumentStore> documentStore = std::make_shared<DocumentStoreSimple>();
	std::shared_ptr<KVStore::IKVStore> invertedIndexStore = std::make_shared<KVStore::KVStoreLevelDb>("/tmp/InvertedIndex");

	engine = new Engine(tokenizer, documentStore, invertedIndexStore, setFactory);
	engine->setMaxBatchSize(200);

	unsigned short port = 8080;

#ifdef _WIN32
	WSADATA WSAData;
	WSAStartup(0x101, &WSAData);
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return (1);
#endif
	if (argc < 2)
	{
		syntax();
		return 1;
	}

	base = event_base_new();
	if (!base)
	{
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		return 1;
	}

	// Create a new evhttp object to handle requests
	http = evhttp_new(base);
	if (!http)
	{
		fprintf(stderr, "couldn't create evhttp. Exiting.\n");
		return 1;
	}

	// The /dump URI will dump all requests to stdout and say 200 ok
	// evhttp_set_cb(http, "/dump", dump_request_cb, NULL);

	evhttp_set_cb(http, SEARCH_PATH.c_str(), search_request_cb, NULL);
	evhttp_set_cb(http, DOC_PATH.c_str(), doc_request_cb, NULL);
	evhttp_set_cb(http, INDEX_PATH.c_str(), post_request_cb, NULL);

	// We want to accept arbitrary requests, so we need to set a "generic"
	evhttp_set_gencb(http, generic_request_cb, argv[1]);

	// Now we tell the evhttp what port to listen on
	handle = evhttp_bind_socket_with_handle(http, "0.0.0.0", port);

	if (!handle)
	{
		fprintf(stderr, "couldn't bind to port %d. Exiting.\n", (int)port);
		return 1;
	}

	printf("Listening on 0.0.0.0:%d\n", port);

	event_base_dispatch(base);

	delete engine;

	return 0;
}

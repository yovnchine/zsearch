
#ifndef DOCUMENTSTORESIMPLE_H
#define DOCUMENTSTORESIMPLE_H

#include "IDocumentStore.h"
#include <map>
#include <memory>
#include "KVStoreLevelDb.h"

using namespace std;

class DocumentStoreSimple : public IDocumentStore
{
	public:
		DocumentStoreSimple();
		
		void addDoc(unsigned int docId, const shared_ptr<IDocument>& doc);

		void removeDoc(unsigned int docId);

		const map<unsigned int, shared_ptr<IDocument>>& getDocuments();
        
		int Get(unsigned int docId, shared_ptr<IDocument>& doc) const;
	private:
		map<unsigned int, shared_ptr<IDocument>> documents;
		// KVStore::KVStore store;
};

#endif

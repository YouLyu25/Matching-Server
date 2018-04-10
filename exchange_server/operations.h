#include <vector>
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <libxml++/libxml++.h>
#include <libxml++/parsers/textreader.h>



int handle_create (xmlpp::TextReader& reader, std::string* response);

int handle_transactions (xmlpp::TextReader& reader, std::string* response);


#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

// network libraries
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// multi-threading library
#include <thread>

// boost library for thread pool
#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>

// XML parser libraries
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <libxml++/libxml++.h>
#include <libxml++/parsers/textreader.h>

// database library
#include <pqxx/pqxx>

#include "operations.h"

#define DEBUG           1
#define DOCKER          0
#define MULTI_THREAD    1
#define NUM_THREAD      16
#define NAME_SIZE       64
#define SERVER_PORT     12345
#define MAX_CONN        200
#define BUFF_SIZE       102400
#define RESIZE_SIZE     1024
#define WAIT_TIME       10

using namespace pqxx;



/*   create table ACCOUNT to store client's information   */
int create_table () {
  try {
    // connect to the database
#if DOCKER
    connection C("dbname=exchange user=postgres password=psql " \
                 "host=exchange_db port=5432");
#else
    connection C("dbname=exchange user=postgres password=psql");
#endif
    std::string sql;
    result R;
    work W(C);
    result::const_iterator res;

    // 1. check if table ACCOUNT exists
    sql = "SELECT COUNT(*) FROM " \
          "information_schema.TABLES WHERE TABLE_NAME='account';";
    R = W.exec(sql);
    res = R.begin();
    if (res[0].as<int>() == 0) { // table does not exist, create table
      sql = "CREATE TABLE ACCOUNT(" \
            "ACCOUNT_ID BIGINT         PRIMARY KEY      NOT NULL, " \
            "BALANCE    NUMERIC(20,2)  NOT NULL         DEFAULT 0 " \
            "CHECK(BALANCE>=0));";
      W.exec(sql);
    }
    
    // 2. check if table OPENED_ORDER exists
    sql = "SELECT COUNT(*) FROM " \
          "information_schema.TABLES WHERE TABLE_NAME='opened_order';";
    R = W.exec(sql);
    res = R.begin();
    if (res[0].as<int>() == 0) { // table does not exist, create table
      sql = "CREATE TABLE OPENED_ORDER(" \
            "ACCOUNT_ID BIGINT         NOT NULL, " \
            "ORDER_ID   BIGINT         NOT NULL, " \
            "SYM        VARCHAR(50)    NOT NULL, " \
            "AMOUNT     BIGINT         NOT NULL, " \
            "PRICE      NUMERIC(20,2)  NOT NULL         CHECK(PRICE>0), " \
            "TIME       BIGINT         NOT NULL         CHECK(TIME>0), " \
            "PRIMARY KEY (ACCOUNT_ID, ORDER_ID));";
      W.exec(sql);
    }
    
    // 3. check if table CLOSED_ORDER exists
    sql = "SELECT COUNT(*) FROM " \
          "information_schema.TABLES WHERE TABLE_NAME='closed_order';";
    R = W.exec(sql);
    res = R.begin();
    if (res[0].as<int>() == 0) { // table does not exist, create table
      sql = "CREATE TABLE CLOSED_ORDER(" \
            "ACCOUNT_ID BIGINT         NOT NULL, " \
            "ORDER_ID   BIGINT         NOT NULL, " \
            "STATUS     INT            NOT NULL, " \
            "SHARES     BIGINT         NOT NULL, " \
            "PRICE      NUMERIC(20, 2) NOT NULL         CHECK(PRICE>0)," \
            "TIME       BIGINT         NOT NULL         CHECK(TIME>0));";
      W.exec(sql);
    }
    
    W.commit();
  }
  catch (std::exception& e) {
    std::cerr << "create_table" << e.what() << std::endl;
    return -1;
  }
  return 0;
}






/*   set server socket   */
int set_socket () {
  int socket_fd;
  int stat;
  char server_hostname[NAME_SIZE];
  int server_port = SERVER_PORT;
  struct sockaddr_in server_addr_info;
  struct hostent* server_info;
  struct timeval wait_time;
  
  gethostname(server_hostname, sizeof(server_hostname));
  server_info = gethostbyname(server_hostname);

  server_addr_info.sin_family = AF_INET;
  server_addr_info.sin_port = htons(server_port);
  server_addr_info.sin_addr.s_addr = INADDR_ANY;
  memcpy(&server_addr_info.sin_addr, server_info->h_addr_list[0], server_info->h_length);
  
  // create TCP socket, bind and listen
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    perror("Cannot create socket");
  }
  
#if 0
  // set timeout for each connection
  wait_time.tv_sec = WAIT_TIME;
  stat = setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO|SO_REUSEADDR,
                    (struct timeval*)&wait_time, sizeof(wait_time));
#else
  int yes = 1;
  stat = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes)); 
#endif
  stat = bind(socket_fd, (struct sockaddr*)&server_addr_info, sizeof(server_addr_info));
  if (stat < 0) {
    perror ("Failed binding");
  }
  stat = listen(socket_fd, MAX_CONN);
  if (stat < 0) {
    perror("Failed listening");
  }
  
  return socket_fd;
}






/*   check if the receiving has finished using the first line (integer) of XML   */
int recv_finished (std::vector <char>& buffer, std::size_t received_bytes) {
  try {
    std::string xml_len;
    std::string xml(buffer.data());
    std::stringstream ss(xml);
    
    // integer on the first line is the length of the XML data which follows it
    ss >> xml_len;
    if (received_bytes == std::stoi(xml_len) + xml_len.length() + 1) { // plus "\n"
      // if finished receiving
      return 1;
    }
  }
  catch (std::exception& e) { // wrong XML format
    std::cerr << "recv_finished: " << e.what() << std::endl;
    return -1;
  }
  return 0; // receiving is not done
}






/*   receive XML data of accepted request   */
int recv_request (int client_conn_sfd, std::vector <char>& buffer) {
  std::size_t len = 0;
  std::size_t i = 0;
  std::size_t received_bytes = 0;
  
  try {  
    while (1) {
      len = recv(client_conn_sfd, &buffer.data()[i], BUFF_SIZE, 0);
      received_bytes += len;
      
      if (recv_finished(buffer, received_bytes) == 1) { // done receiving request
        break;
      }
      else if (recv_finished(buffer, received_bytes) == -1) {
        std::cerr << "invalid request" << std::endl;
        return -1; // wrong format, exit thread
      }
      else { // size isn't enough, resize and continue receiving
        if (received_bytes == buffer.size()) {
          buffer.resize(received_bytes + RESIZE_SIZE);
          i = received_bytes;
        }
        else if (len == 0) { // 0 received, connection is closed
          if (received_bytes != 0) {
            break;
          }
          else { // no request received, close connection and exit
            perror("no request");
            return -1;
          }
        }
        else if (len < 0) { // error
          perror("request recv");
          return -1;
        }
        else { // normally should never enter here
          std::cerr << "invalid request" << std::endl;
          return -1;
        }
      }
    }
  }
  catch (std::exception& e) {
    std::cerr << "recv_request: " << e.what() << std::endl;
    return -1;
  }
  return received_bytes;
}






/*   remove first two lines and all "\n"   */
int parse_xml_simple (std::string& xml) {
  try {
    std::stringstream ss;
    std::size_t pos;
    std::string line;
    
    if ((pos = xml.find("\n")) != std::string::npos) {
      // ignore the first line (integer)
      xml = xml.substr(pos+1);
    }
    else {
      return -1; // invalid format
    }
  
    if ((pos = xml.find("<create>\n")) != std::string::npos) {
      ss << xml.substr(pos);
      xml = xml.substr(pos+9); // remove <create>
      if ((pos = xml.find("<transactions")) != std::string::npos) {
        return -1; // cannot have both create and transactions
      }
      if ((pos = xml.find("<create>\n")) != std::string::npos) {
        return -1; // cannot have more than one <create>
      }
    }
    else if ((pos = xml.find("<transactions")) != std::string::npos) {
      ss << xml.substr(pos);
      std::stringstream temp_ss(xml.substr(pos));
      std::string temp;
      std::getline(temp_ss, temp, '\n');
      std::getline(temp_ss, temp, '\n');
      pos = xml.find(temp);
      xml = xml.substr(pos);
      if ((pos = xml.find("<transactions")) != std::string::npos) {
        return -1; // cannot have more than one <transactions>
      }
    }
    
    if (xml.find("<account") == std::string::npos &&
        xml.find("<symbol") == std::string::npos &&
        xml.find("<order") == std::string::npos &&
        xml.find("<cancel") == std::string::npos &&
        xml.find("<query") == std::string::npos) {
      return -1; // no child
    }
    
    xml = "";
    // remove all "\n" for parsing purpose
    while (getline(ss, line, '\n')) {
      xml += line;
    }
  }
  catch (std::exception& e) {
    std::cerr << "parse_xml_simple: " << e.what() << std::endl;
    return -1;
  }
  return 0;
}





/*   parse and execute the operation of request   */
void execute_request (std::vector <char>& buffer, std::string& response) {
  try {
    std::string id;
    std::string balance;
    std::string sym;
    std::string shares;
    std::string xml(buffer.data());
    int stat;
    
    if (parse_xml_simple(xml) < 0) {
      response = "<results>\n  <error>Invalid XML request</error>\n</results>\n";
      return;
    }
    xmlpp::TextReader reader((const unsigned char*)xml.c_str(), xml.length());
    reader.read();
    // create, account, symbol or NUM of shares (<account id=\"12\">60</account>)
    if (reader.get_name() == "create") { // handle <create>
      stat = handle_create(reader, response);
      if (stat == -1) { // invalid XML request
        response = "<results>\n  <error>Invalid XML request</error>\n</results>\n";
      }
      else if (stat == -2) { // unexpected exception caught
        response = "<results>\n  <error>unexpected exception</error>\n</results>\n";
      }
      else {
        response = "<results>\n" + response + "</results>\n";
      }
    }
    else if (reader.get_name() == "transactions") { // handle <transactions>
      stat = handle_transactions(reader, response);
      if (stat == -1) {
        response = "<results>\n  <error>Invalid XML request</error>\n</results>\n";
      }
      else if (stat == -2) { // unexpected exception caught
        response = "<results>\n  <error>unexpected exception</error>\n</results>\n";
      }
      else if (stat == -3) { // unexpected exception caught
        response = "<results>\n  <error>account does not exist</error>\n</results>\n";
      }
      else {
        response = "<results>\n" + response + "</results>\n";
      }
    }
    else { // invalid request
      response = "<results>\n  <error>Invalid XML request</error>\n</results>\n";
    }
  }
  catch (std::exception& e) {
    std::cerr << "execute_request: " << e.what() << std::endl;
    response = "<results>\n  <error>Invalid XML request</error>\n</results>\n";
  }
  return;
}






/*   handle accepted request   */
void handle_request (int request_id, int client_conn_sfd) {
  try {
    std::vector <char> buffer(BUFF_SIZE);
    std::vector <std::string>* response = new std::vector <std::string> (0);
    std::string response_str = "";
    int received_bytes;
    int stat;
    
    std::cout << "request_id: " << request_id << ", client_conn_sfd: "
              << client_conn_sfd << "\n" << std::endl;
    
    received_bytes = recv_request(client_conn_sfd, buffer);
    if (received_bytes <= 0) { // invalid XML request
      response_str = "<result>\n  <error>Invalid XML request</error>\n</result>\n";
      response_str = std::to_string(response_str.length()) + "\n" + 
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + response_str;
    }
    else {
      // parse and execute request
      execute_request(buffer, response);
      response_str = std::to_string(response_str.length()) + "\n" + 
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + response_str;
    }
    
    for (int i = 0; i < response->size(); ++i) {
      response_str += *response[i];
    }
    
    // send resulting XML response
    int len = send(client_conn_sfd, response_str.c_str(),
                   response_str.length(), 0);
    delete response;
    close(client_conn_sfd);
  }
  catch (std::exception& e) {
    std::cerr << "handle_request: " << e.what() << std::endl;
  }
}






/*   MAIN   */
int main () {
  int server_sfd = set_socket(); 
  int thread_id = 0;
  
  if (create_table() < 0) { // failed to create table
    return EXIT_FAILURE;
  }
  
  // thread pool with maximum NUM_THREAD concurrently running threads
  boost::asio::thread_pool handler(NUM_THREAD);
  while (1) {
    try {
      struct sockaddr_in client_addr;
      socklen_t addr_len = sizeof(client_addr);
      int client_conn_sfd = accept(server_sfd,
                                  (struct sockaddr*)&client_addr,
                                  &addr_len);
      
      if (client_conn_sfd == -1) {
        perror("Cannot accept connection");
        continue;
      }
#if MULTI_THREAD
      //std::thread(handle_request, thread_id, client_conn_sfd).detach();
      boost::asio::post(handler, boost::bind(handle_request,
                                             thread_id, client_conn_sfd));
#else
      handle_request(thread_id, client_conn_sfd);
#endif
      ++thread_id;
    }
    catch (std::exception& e) {
      std::cerr << "main: " << e.what() << std::endl;
    }
  }
  handler.join();
  close (server_sfd);
  return EXIT_SUCCESS;
}




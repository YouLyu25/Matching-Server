#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <memory>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <libxml++/libxml++.h>
#include <libxml++/parsers/textreader.h>

// boost library for thread pool
#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>

// database library
#include <pqxx/pqxx>

#define DEBUG           1
#define DOCKER          1
#define THREAD_POOL     1
#define NUM_THREAD      1

using namespace pqxx;



/*   add new account to the database   */
void create_account (std::string id, std::string balance, std::string* response) {
  try {
    std::string sql;
    result R;
    result::const_iterator res;
    // connect to the database
    // exchange_db is the host name used between containers
#if DOCKER
    connection C("dbname=exchange user=postgres password=psql " \
                 "host=exchange_db port=5432");
#else
    connection C("dbname=exchange user=postgres password=psql ");
#endif
    work W(C);
    
    try {
      if (std::stoll(id) < 0) { // invalid account number
        *response += "  <error id=\"" + id + "\">Invalid account number</error>\n";
        return;
      }
      else if (std::stold(balance) < 0) { // invalid balance value
        *response += "  <error id=\"" + id + "\">Invalid balance value</error>\n";
        return;
      }
    }
    catch (std::exception& e) { // invalid account or balance format
      *response += "  <error id=\"" + id + "\">Invalid account or balance</error>\n";
      return;
    }
    // check if the account already exists
    sql = "SELECT COUNT(ACCOUNT_ID) FROM ACCOUNT WHERE ACCOUNT_ID = "
          + W.quote(id) + ";";
    R = W.exec(sql);
    res = R.begin();
    if (res[0].as<int>() != 0) { // account already exists, response <error>
      *response += "  <error id=\"" + id + "\">Account already exists</error>\n";
      return;
    }
    
    // account does not exist, create new account
    sql = "INSERT INTO ACCOUNT (ACCOUNT_ID, BALANCE) VALUES (";
    sql += W.quote(id) + ", ";
    sql += W.quote(balance) + ");";
    W.exec(sql);
    W.commit();
    C.disconnect();
  }
  catch (std::exception& e) {
    std::cerr << "create_account: " << e.what() << std::endl;
    *response = "  <error id=\"" + id + "\">" + "Invalid request...</error>\n";
    return;
  }
  // successfully created, response <created>
  *response += "  <created id=\"" + id + "\"/>\n";
  return;
}






/*   add symbol shares to specific account(s)   */
void add_shares (std::string sym, std::vector<std::string> id_arr,
                std::vector<std::string> num_shares_arr, std::string* response) {
  try {
    std::string sql;
    result R;
    // connect to the database
    // exchange_db is the host name used between containers
#if DOCKER
    connection C("dbname=exchange user=postgres password=psql " \
                 "host=exchange_db port=5432");
#else
    connection C("dbname=exchange user=postgres password=psql ");
#endif
    work W(C);

    // check if the column indicated by sym exists
    sql = "SELECT COLUMN_NAME FROM information_schema.COLUMNS "\
          "WHERE TABLE_NAME = 'account';";
    R = W.exec(sql);
    bool column_exist = false;
    for (result::const_iterator res = R.begin(); res != R.end(); ++res) {
      for (result::tuple::const_iterator field = res->begin();
           field != res->end(); ++field) {
        if (field->c_str() == sym) {
          column_exist = true;
          break; // column exists, break;
        }
      }
    }
    if (column_exist == false) { // column indicated by sym does not exist
      // add new column, shares of a symbol should not be negative
      sql = "ALTER TABLE ACCOUNT ADD COLUMN \"" + sym +
            "\" BIGINT NOT NULL DEFAULT 0 CHECK(\"" + sym + "\">=0);";
      W.exec(sql);
    }
    
    std::string temp_response;
    for (std::size_t i = 0; i < id_arr.size(); ++i) {
      // check if the account exists
      sql = "SELECT ACCOUNT_ID FROM ACCOUNT WHERE ACCOUNT_ID = " +
            W.quote(id_arr[i]);
      R = W.exec(sql);
      result::const_iterator res = R.begin();
      if (res == R.end()) { // account does not exist
        //W.commit();
        temp_response += "    <error id=\"" + id_arr[i] +
                         "\">Account does not exist</error>\n";
        continue;
      }
      
      // add new symbol shares, should be non-negative
      long long updated_shares;
      // get current shares first
      sql = "SELECT \"" + sym + "\" FROM ACCOUNT WHERE ACCOUNT_ID = " +
            W.quote(id_arr[i]) + ";";
      R = W.exec(sql);
      res = R.begin();
      long long curr_shares = res[0].as<long long>();
      // check if net shares is negative
      updated_shares = curr_shares + std::stoll(num_shares_arr[i]);
      if (updated_shares < 0) { // negative value
        temp_response += "    <error id=\"" + id_arr[i] +
                         "\">Negative share value</error>\n";
        continue;
      }
      
      // update shares of sym
      sql = "UPDATE ACCOUNT SET \"" + sym + "\" = " +
            std::to_string(updated_shares) + " WHERE ACCOUNT_ID = " +
            W.quote(id_arr[i]) + ";";
      W.exec(sql);
      temp_response += "    <created id=\"" + id_arr[i] + "\"/>\n";
    }
    // generate response XML
    if (temp_response.find("<error") == std::string::npos) { // if no <error>
      *response = *response + "  <created sym=\"" + sym + "\">\n" +
                 temp_response + "  </created>\n";
    }
    else { // if there is <error>, use <error sym="XX">
      *response = *response + "  <error sym=\"" + sym + "\">\n" +
                 temp_response + "  </error>\n";
    }
    W.commit();
    C.disconnect();
  }
  catch (std::exception& e) { // exception caught, generate response XML
    std::cerr << "add_shares: " << e.what() << std::endl;
    *response = "  <error sym=\"" + sym + "\">\n";
    for (std::size_t i = 0; i < id_arr.size(); ++i) {
      *response += "    <error id=\"" + id_arr[i] + "\">" +
                   "Invalid request...</error>\n";
    }
    *response += "  </error>\n";
  }
  return;
}






/*   if root node of XML is <create>   */
int handle_create (xmlpp::TextReader& reader, std::string* response) {
  try {
    std::string id;
    std::string balance;
    std::string sym;
    bool create_open = false;
    boost::asio::thread_pool handler(NUM_THREAD);
    
    do {
      if (reader.get_name() == "create") {
        if (create_open == true) {
          break; // encounter </create>, finish
        }
        create_open = true;
      }
      else if (reader.get_name() == "account") { // if node's name is "account"
        if (reader.get_attribute_count() == 2) { // if node has 2 attribute
          reader.move_to_first_attribute();
          if (reader.get_name() == "id") { // 1st attribute should be "id"
            id = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          
          reader.move_to_next_attribute();
          if (reader.get_name() == "balance") { // 2nd attribute should be "balance"
            balance = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
#if THREAD_POOL
          boost::asio::post(handler, boost::bind(create_account,
                                                 id, balance, response));
#else
          create_account(id, balance, response); // create account
#endif
        }
        else { // not 2 attributes, invalid XML request
          return -1;
        }
      }
      
      
      /*   <symbol sym="SPY">                <--- starts here
             <account id="123">100</account>
             <account id="456">200</account>
           </symbol>   */
      else if (reader.get_name() == "symbol") { // if node's name is "symbol"
        if (reader.get_attribute_count() == 1) { // if node has 1 attribute (sym)
          reader.move_to_first_attribute();
          if (reader.get_name() == "sym") { // 1st attribute should be "sym"
            sym = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          
          std::vector <std::string> id_arr;
          std::vector <std::string> num_shares_arr;
          while (1) { // get all <account id="xxx">NUM</account>
            reader.read();
            if (reader.get_name() == "symbol") { // encounter </symbol>, finish
              break;
            }
            else if (reader.get_name() == "account") {
              if (reader.get_attribute_count() == 1) { // can only have attribute "id"
                reader.move_to_first_attribute();
                id_arr.push_back(reader.get_value()); // add id
              }
              else {
                return -1; // invalid XML request
              }
              reader.read();
              if (reader.get_name() == "#text") { // NUM of shares field
                num_shares_arr.push_back(reader.get_value()); // add NUM of shares
                reader.read(); // read one more time to skip </account>
                if (reader.get_name() != "account") {
                  return -1; // invalid XML request
                }
              }
              else {
                return -1; // invalid XML request
              }
            }
            else if (reader.get_name() == "#text") {
              continue; // ignore spaces
            }
            else {
              return -1; // invalid XML request
            }
          }
#if THREAD_POOL
          boost::asio::post(handler, boost::bind(add_shares,
                                                 sym, id_arr,
                                                 num_shares_arr, response));
#else
          add_shares(sym, id_arr, num_shares_arr, response);
#endif
        }
        else {
          return -1; // invalid XML request
        }
      }
      else if (reader.get_name() == "#text") {
        continue; // ignore spaces
      }
      else {
        return -1; // invalid XML request
      }
    } while (reader.read());
    handler.join();
  }
  catch (std::exception& e) {
    std::cerr << "handle_create: " << e.what() << std::endl;
    return -2; // unexpected exception
  }
  return 0;
}




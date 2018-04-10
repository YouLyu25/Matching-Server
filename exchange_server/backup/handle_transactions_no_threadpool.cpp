#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <memory>
#include <iomanip>

#include <time.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <libxml++/libxml++.h>
#include <libxml++/parsers/textreader.h>

// database library
#include <pqxx/pqxx>

#define DEBUG   1
#define DOCKER  0
#define SELL    0
#define BUY     1

using namespace pqxx;



/*   function to control format of data which will be stored to the database  */
std::string set_precision (long double ld) {
  std::string str = std::to_string(ld);
  std::size_t pos = str.find(".");
  str = str.substr(0, pos+3);
  return str;
}






/*   update transaction records including balance, amount and finished orders   */
int update_record (connection* C, work& W, int status, std::string& sym,
                   std::string& seller_account_id, std::string& buyer_account_id,
                   long long& matched_amount_ld, long double& matched_limit_ld) {
  try {
    std::string sql;
    result R;
    result::const_iterator res;
    std::string seller_order_id;
    std::string buyer_order_id;
    long double seller_curr_balance_ld;
    long double seller_new_balance_ld;
    long long seller_curr_shares_ld;
    long long seller_new_shares_ld;
    long long seller_curr_amount_ld;
    long long seller_new_amount_ld;
    long double buyer_curr_balance_ld;
    long double buyer_new_balance_ld;
    long long buyer_curr_shares_ld;
    long long buyer_new_shares_ld;
    long long buyer_curr_amount_ld;
    long long buyer_new_amount_ld;
    long double balance_diff_ld;
    long long amount_diff_ld;
    
    /*   1. update seller and buyer's accounts (ACCOUNT)  */
    // get seller's current balance and sym shares
    sql = "SELECT BALANCE, \"" + sym + "\" FROM ACCOUNT WHERE ACCOUNT_ID = " +
          W.quote(seller_account_id) + ";";
    R = W.exec(sql);
    /*   TODO: pay attention to possible segfault   */
    res = R.begin();
    if (res == R.end()) {
      std::cerr << "6" << std::endl;
    }
    seller_curr_balance_ld = res[0].as<long double>();
    seller_curr_shares_ld = res[1].as<long long>();
    
    // get buyer's current balance and sym shares
    sql = "SELECT BALANCE, \"" + sym + "\" FROM ACCOUNT WHERE ACCOUNT_ID = " +
          W.quote(buyer_account_id) + ";";
    R = W.exec(sql);
    /*   TODO: pay attention to possible segfault   */
    res = R.begin();
    if (res == R.end()) {
      std::cerr << "7" << std::endl;
    }
    buyer_curr_balance_ld = res[0].as<long double>();
    buyer_curr_shares_ld = res[1].as<long long>();
    
    // calculate seller and buyer's new account balance
    balance_diff_ld = matched_amount_ld * matched_limit_ld;
    seller_new_balance_ld = seller_curr_balance_ld + balance_diff_ld;
    seller_new_shares_ld = seller_curr_shares_ld - matched_amount_ld;
    buyer_new_balance_ld = buyer_curr_balance_ld - balance_diff_ld;
    buyer_new_shares_ld = buyer_curr_shares_ld + matched_amount_ld;
    
    if (status == SELL) {
      // update buyer's account
      sql = "UPDATE ACCOUNT SET \"" + sym + "\" = " +
            W.quote(std::to_string(buyer_new_shares_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(buyer_account_id) + ";";
      W.exec(sql);
    }
    else { // status == BUY
      // update seller's account
      sql = "UPDATE ACCOUNT SET BALANCE = " +
            W.quote(set_precision(seller_new_balance_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(seller_account_id) + ";";
      W.exec(sql);
    }
    
    
    
    /*   2. update record of opened orders (OPENED_ORDER)  */
    // get seller's current amount
    if (status == SELL) {
      // get buyer's current amount
      sql = "SELECT ORDER_ID, AMOUNT FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
            W.quote(buyer_account_id) + " AND SYM = " + W.quote(sym) +
            " AND PRICE = " + W.quote(set_precision(matched_limit_ld)) +
            " ORDER BY TIME ASC;";
      R = W.exec(sql);
      /*   TODO: pay attention to possible segfault   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "8: " + sql << std::endl;
      }
      buyer_order_id = res[0].as<std::string>();
      buyer_curr_amount_ld = res[1].as<long long>();
      buyer_new_amount_ld = buyer_curr_amount_ld - matched_amount_ld;
      // update buyer's opened order
      sql = "UPDATE OPENED_ORDER SET AMOUNT =" +
            W.quote(std::to_string(buyer_new_amount_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(buyer_account_id) +
            " AND ORDER_ID = " + W.quote(buyer_order_id) + ";";
    }
    else { // status == BUY
      // get seller's current amount
      sql = "SELECT ORDER_ID, AMOUNT FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
            W.quote(seller_account_id) + " AND SYM = " + W.quote(sym) +
            " AND AMOUNT < 0 AND PRICE <= " + W.quote(set_precision(matched_limit_ld)) +
            " ORDER BY PRICE ASC, TIME ASC;";
      R = W.exec(sql);
      /*   TODO: pay attention to possible segfault   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "9: " + sql << std::endl;
      }
      seller_order_id = res[0].as<std::string>();
      seller_curr_amount_ld = res[1].as<long long>();
      // seller's amount should be negative to indicate "sell"
      seller_new_amount_ld = -(-seller_curr_amount_ld - matched_amount_ld);
      // update seller's opened order
      sql = "UPDATE OPENED_ORDER SET AMOUNT =" +
            W.quote(std::to_string(seller_new_amount_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(seller_account_id) +
            " AND ORDER_ID = " + W.quote(seller_order_id) + ";";
    }
    W.exec(sql);
    
    
    
    /*   3. update record of finished orders (CLOSED_ORDER)   */
    if (status == SELL) {
      // get current number of orders for seller
      sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
            W.quote(seller_account_id) + ";";
      R = W.exec(sql);
      res = R.begin();
      // seller order id is his current number of opened orders plus 1
      seller_order_id = std::to_string(res[0].as<long long>()+1);
    }
    else { // status == BUY
      // get current number of orders for buyer
      sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
            W.quote(buyer_account_id) + ";";
      R = W.exec(sql);
      res = R.begin();
      // buyer order id is his current number of opened orders plus 1
      buyer_order_id = std::to_string(res[0].as<long long>()+1);
    }
    // update seller and buyer's finished order records
    time_t curr_time = time(NULL);
    sql = "INSERT INTO CLOSED_ORDER ";
    sql += "(ACCOUNT_ID, ORDER_ID, STATUS, SHARES, PRICE, TIME) VALUES (" +
           W.quote(seller_account_id) + ", " +
           W.quote(seller_order_id) + ", " +
           "0, " + // 0 indicate it is executed (1 is canceled)
           W.quote(std::to_string(-matched_amount_ld)) + ", " +
           W.quote(set_precision(matched_limit_ld)) + ", " +
           W.quote(std::to_string(curr_time)) + ");";
    W.exec(sql);
    curr_time = time(NULL);
    sql = "INSERT INTO CLOSED_ORDER ";
    sql += "(ACCOUNT_ID, ORDER_ID, STATUS, SHARES, PRICE, TIME) VALUES (" +
           W.quote(buyer_account_id) + ", " +
           W.quote(buyer_order_id) + ", " +
           "0, " + // 0 indicate it is executed (1 is canceled)
           W.quote(std::to_string(matched_amount_ld)) + ", " +
           W.quote(set_precision(matched_limit_ld)) + ", " +
           W.quote(std::to_string(curr_time)) + ");";
    W.exec(sql);
  }
  catch (std::exception& e) {
    std::cerr << "update_record: " << e.what() << std::endl;
    return -1;
  }
  return 0;
}






/*   match order   */
// NOTE: reference to return_order_id in declaration should not be modified
int match_order (connection* C, work& W, std::string& return_order_id,
                 std::string& account_id, std::string& sym,
                 std::string& amount, std::string& limit,
                 std::string& response) {
  // find matching from database
  try {
    std::string sql;
    result R;
    result::const_iterator res;
    long long amount_ld = std::stoll(amount);
    long double limit_ld = std::stold(limit);
    long long buyer_amount_ld;
    long double buyer_limit_ld;
    long long seller_amount_ld;
    long double seller_limit_ld;
    long long matched_amount_ld;
    long double matched_limit_ld;
    std::string buyer_account_id;
    std::string seller_account_id;
    std::string order_id;
    
    /*   sell goods, find buyers   */
    if (amount_ld < 0) {
      seller_account_id = account_id;
      seller_amount_ld = -amount_ld;
      seller_limit_ld = limit_ld;
      
      sql = "SELECT * FROM OPENED_ORDER WHERE SYM = " + W.quote(sym) +
            " AND ACCOUNT_ID != " + W.quote(seller_account_id) +
            " AND AMOUNT > 0 AND PRICE >= " + W.quote(limit) +
            " ORDER BY PRICE DESC, TIME ASC;";
      R = W.exec(sql);
      res = R.begin();
      
      // if there is no match
      if (res == R.end()) {
        // get current number of orders for seller
        sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
              W.quote(seller_account_id) + ";";
        R = W.exec(sql);
        res = R.begin();
        // seller order id is his current number of opened orders plus 1
        order_id = std::to_string(res[0].as<long long>()+1);
        return_order_id = order_id;
        
        time_t curr_time = time(NULL);
        // store order into database for future match
        sql = "INSERT INTO OPENED_ORDER ";
        sql += "(ACCOUNT_ID, ORDER_ID, SYM, AMOUNT, PRICE, TIME) VALUES (";
        sql += W.quote(seller_account_id) + ", ";
        sql += W.quote(order_id) + ", ";
        sql += W.quote(sym) + ", ";
        sql += W.quote(std::to_string(-seller_amount_ld)) + ", ";
        sql += W.quote(limit) + ", ";
        sql += W.quote(std::to_string(curr_time)) + ");";
        W.exec(sql);
        
        return 0;
      }
      
      // if there is a match
      for (res = R.begin(); res != R.end(); ++res) {
        buyer_account_id = res[0].as<std::string>();
        matched_amount_ld = res[3].as<long long>();
        matched_limit_ld = res[4].as<long double>();
        if (seller_amount_ld > matched_amount_ld) {
          seller_amount_ld -= matched_amount_ld;
          if (update_record(C, W, SELL, sym, seller_account_id, buyer_account_id,
                            matched_amount_ld, matched_limit_ld) < 0) {
            return -1;
          }
        }
        else { // goods all sold
          matched_amount_ld = seller_amount_ld;
          seller_amount_ld = 0; // seller's good sold out
          if (update_record(C, W, SELL, sym, seller_account_id, buyer_account_id,
                            matched_amount_ld, matched_limit_ld) < 0) {
            return -1;
          }
          break;
        }
      }
      
      //   update amount in case there is un-finished order
#if 0
      if (seller_amount_ld != 0) { // there is still unfinished order
#else
      if (1) {
#endif
        // get current number of orders for seller
        sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
              W.quote(seller_account_id) + ";";
        R = W.exec(sql);
        res = R.begin();
        // seller order id is his current number of opened orders plus 1
        order_id = std::to_string(res[0].as<long long>()+1);
        return_order_id = order_id;
        
        time_t curr_time = time(NULL);
        // store order into database for future match
        sql = "INSERT INTO OPENED_ORDER ";
        sql += "(ACCOUNT_ID, ORDER_ID, SYM, AMOUNT, PRICE, TIME) VALUES (";
        sql += W.quote(seller_account_id) + ", ";
        sql += W.quote(order_id) + ", ";
        sql += W.quote(sym) + ", ";
        sql += W.quote(std::to_string(-seller_amount_ld)) + ", ";
        sql += W.quote(limit) + ", ";
        sql += W.quote(std::to_string(curr_time)) + ");";
        W.exec(sql);
      }
    }
    
    
    
    /*   purchase goods, find sellers   */
    else {
      buyer_account_id = account_id;
      buyer_amount_ld = amount_ld;
      buyer_limit_ld = limit_ld;
      
      sql = "SELECT * FROM OPENED_ORDER WHERE SYM = " + W.quote(sym) +
            " AND ACCOUNT_ID != " + W.quote(buyer_account_id) +
            " AND AMOUNT < 0 AND PRICE <= " + W.quote(limit) +
            " ORDER BY PRICE ASC, TIME ASC;";
      R = W.exec(sql);
      res = R.begin();
      
      // if there is no match
      if (res == R.end()) {
        // get current number of orders for seller
        sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
              W.quote(buyer_account_id) + ";";
        R = W.exec(sql);
        res = R.begin();
        // seller order id is his current number of opened orders plus 1
        order_id = std::to_string(res[0].as<long long>()+1);
        return_order_id = order_id;
        
        time_t curr_time = time(NULL);
        // store order into database for future match
        sql = "INSERT INTO OPENED_ORDER ";
        sql += "(ACCOUNT_ID, ORDER_ID, SYM, AMOUNT, PRICE, TIME) VALUES (";
        sql += W.quote(buyer_account_id) + ", ";
        sql += W.quote(order_id) + ", ";
        sql += W.quote(sym) + ", ";
        sql += W.quote(std::to_string(buyer_amount_ld)) + ", ";
        sql += W.quote(limit) + ", ";
        sql += W.quote(std::to_string(curr_time)) + ");";
        W.exec(sql);
        
        return 0;
      }
      
      // if there is a match
      for (res = R.begin(); res != R.end(); ++res) {
        seller_account_id = res[0].as<std::string>();
        matched_amount_ld = -res[3].as<long long>();
        //matched_limit_ld = res[4].as<long double>();
        matched_limit_ld = std::stold(limit);
        
        if (buyer_amount_ld > matched_amount_ld) {
          buyer_amount_ld -= matched_amount_ld;
          if (update_record(C, W, BUY, sym, seller_account_id, buyer_account_id,
                            matched_amount_ld, matched_limit_ld) < 0) {
            return -1;
          }
        }
        else { // goods all sold
          matched_amount_ld = buyer_amount_ld;
          buyer_amount_ld = 0; // seller's good sold out
          if (update_record(C, W, BUY, sym, seller_account_id, buyer_account_id,
                            matched_amount_ld, matched_limit_ld) < 0) {
            return -1;
          }
          break;
        }
      }
      /*   update amount in case there is un-finished order   */
#if 0
      if (buyer_amount_ld != 0) { // there is still unfinished order
#else
      if (1) {
#endif
        // get current number of orders for seller
        sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
              W.quote(buyer_account_id) + ";";
        R = W.exec(sql);
        res = R.begin();
        // seller order id is his current number of opened orders plus 1
        order_id = std::to_string(res[0].as<long long>()+1);
        return_order_id = order_id;
        
        time_t curr_time = time(NULL);
        // store order into database for future match
        sql = "INSERT INTO OPENED_ORDER ";
        sql += "(ACCOUNT_ID, ORDER_ID, SYM, AMOUNT, PRICE, TIME) VALUES (";
        sql += W.quote(buyer_account_id) + ", ";
        sql += W.quote(order_id) + ", ";
        sql += W.quote(sym) + ", ";
        sql += W.quote(std::to_string(buyer_amount_ld)) + ", ";
        sql += W.quote(limit) + ", ";
        sql += W.quote(std::to_string(curr_time)) + ");";
        W.exec(sql);
      }
    }
  }
  catch (std::exception& e) {
    std::cerr << "match_order: " << e.what() << std::endl;
    return -1;
  }
  return 0;
}






/*   place incoming order and check if there is a match   */
void place_order (connection* C, std::string& account_id,
                  std::string& sym, std::string& amount,
                  std::string& limit, std::string& response) {
  try {
    std::string sql;
    work W(*C);
    result R;
    result::const_iterator res;
    long long amount_ld;
    long double limit_ld = std::stold(limit);
    std::string order_id;
    
    try {
      // check if the amount value is valid
      amount_ld = std::stoll(amount);
      if (amount_ld == 0) { // invalid amount
        response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                    "\" limit=\"" + limit + "\">Invalid amount</error>\n";
        return;
      }
      if (std::stold(limit) <= 0) { // invalid price
        response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                    "\" limit=\"" + limit + "\">Invalid limit</error>\n";
        return;
      }
    }
    catch (std::exception& e) { // invalid account or balance format
      response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                  "\" limit=\"" + limit + "\">\n" +
                  "    Invalid amount or limit\n  </error>\n";
      return;
    }
#if 1 
    // check if the symbol is currently in the market
    sql = "SELECT COLUMN_NAME FROM information_schema.COLUMNS "\
          "WHERE TABLE_NAME = 'account';";
    R = W.exec(sql);
    bool column_exist = false;
    for (res = R.begin(); res != R.end(); ++res) {
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
#endif
    
    // check if seller's sym share is enough
    if (amount_ld < 0) { // SELL
      sql = "SELECT \"" + sym + "\" FROM ACCOUNT WHERE ACCOUNT_ID = ";
      sql += W.quote(account_id) + ";";
      R = W.exec(sql);
      /*   TODO: pay attention to possible segfault   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "1" << std::endl;
      }
      long long new_shares_ld = res[0].as<long long>() + amount_ld;
      
      if (new_shares_ld < 0) {
        // insufficient shares, cannot place order
        response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                    "\" limit=\"" + limit + "\">\n" +
                    "    Shares of symbol not enough\n  </error>\n";
        return;
      }
      
      // deduce shares from seller's account
      sql = "UPDATE ACCOUNT SET \"" + sym + "\" = " +
            W.quote(std::to_string(new_shares_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(account_id) + ";";
      W.exec(sql);
    }
    
    else { // BUY
      // get current balance, check if there is enough funds
      sql = "SELECT BALANCE FROM ACCOUNT WHERE ACCOUNT_ID = " +
            W.quote(account_id) + ";";
      R = W.exec(sql);
      /*   TODO: pay attention to possible segfault   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "2" << std::endl;
      }
      /*   new balance ha?   */
      long double new_balance_ld = res[0].as<long double>() - amount_ld * limit_ld;
      if (new_balance_ld < 0) {
        // insufficient funds, cannot place order
        response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                    "\" limit=\"" + limit + "\">\n" +
                    "    Insufficient funds\n  </error>\n";
        return;
      }
      
      // update balance of buyer's account
      sql = "UPDATE ACCOUNT SET BALANCE = " +
            W.quote(set_precision(new_balance_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(account_id) + ";";
      W.exec(sql);
    }
    
    // match order and update records
    int stat = match_order(C, W, order_id, account_id,
                           sym, amount, limit, response);
    if (stat == -1) {
      response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                  "\" limit=\"" + limit + "\">\n" +
                  "    Unable to update record\n  </error>\n";
      return;
    }
    else if (stat == -2) {
      response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                  "\" limit=\"" + limit + "\">\n" +
                  "    Unable to match order\n  </error>\n";
      return;
    }
    response += "  <opened id=\"" + order_id + "\" sym=\"" + sym +
                "\" amount=\"" + amount + "\" limit=\"" + limit + "\"/>\n";
    W.commit();
  }
  catch (std::exception& e) {
    std::cerr << "place_order: " << e.what() << std::endl;
    response += "  <error sym=\"" + sym + "\" amount=\"" + amount +
                "\" limit=\"" + limit + "\">Invalid request!</error>\n";
    return;
  }
  return;
}






/*   look for order records   */
void query_order (connection* C, std::string& account_id,
                  std::string& order_id, std::string& response) {
  try {
    std::string sql;
    work W(*C);
    result R;
    result::const_iterator res;
    bool canceled = false;
    long long opened_amount_ld;
    
    // check if the order exists
    sql = "SELECT COUNT(*) FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
          W.quote(account_id) + " AND ORDER_ID = " + W.quote(order_id) + ";";
    R = W.exec(sql);
    res = R.begin();
    if (res[0].as<int>() == 0) { // order queried does not exist
      response += "  <error id=\"" + order_id +
                "\">Order does not exist</error>\n";
      return;
    }
    
    // check if the order is canceled
    sql = "SELECT STATUS FROM CLOSED_ORDER WHERE ACCOUNT_ID = " +
          W.quote(account_id) + " AND ORDER_ID = " + W.quote(order_id) + ";";
    R = W.exec(sql);
    for (res = R.begin(); res != R.end(); ++res) {
      if (res[0].as<int>() == 1) { // order has been canceled
        canceled = true;
        break;
      }
    }
    
    response += "  <status id=\"" + order_id + "\">\n";
    sql = "SELECT * FROM CLOSED_ORDER WHERE ACCOUNT_ID = " + W.quote(account_id) +
          " AND ORDER_ID = " + W.quote(order_id) + ";";
    R = W.exec(sql);
    if (canceled == false) { // no canceling record
      for (res = R.begin(); res != R.end(); ++res) {
        response += "    <executed shares=\"" + res[3].as<std::string>() +
                    "\" price=\"" + res[4].as<std::string>() +
                    "\" time=\"" + res[5].as<std::string>() + "\"/>\n";
      }
      // check if the order is still open
      sql = "SELECT AMOUNT, PRICE FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
            W.quote(account_id) + " AND ORDER_ID = " + W.quote(order_id) + ";";
      R = W.exec(sql);
      /*   TODO   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "3" << std::endl;
      }
      opened_amount_ld = res[0].as<long long>();
      if (opened_amount_ld != 0) { // the order is still open
        response += "    <open shares=\"" + res[0].as<std::string>() +
                    "\"/>\n";
      }
    }
    else { // order has been canceled
      for (res = R.begin(); res != R.end(); ++res) {
        if (res[2].as<int>() == 0) { // executed order
          response += "    <executed shares=\"" + res[3].as<std::string>() +
                      "\" price=\"" + res[4].as<std::string>() +
                      "\" time=\"" + res[5].as<std::string>() + "\"/>\n";
        }
        else { // canceled order
          response += "    <canceled shares=\"" + res[3].as<std::string>() +
                      "\" time=\"" + res[5].as<std::string>() + "\"/>\n";
          break; // ought to be the last one
        }
      }
    }
    response += "  </status>\n";
    W.commit();
  }
  catch (std::exception& e) {
    std::cerr << "query_order: " << e.what() << std::endl;
    response += "  <error id=\"" + order_id +
                "\">Unable to query order</error>\n";
    return;
  }
}






/*   cancel opened order, i.e. update OPENED_ORDER and CLOSED_ORDER   */
void cancel_order (connection* C, std::string& account_id,
                  std::string& order_id, std::string& response) {
  try {
    std::string sql;
    work W(*C);
    result R;
    result::const_iterator res;
    std::string sym;
    long long opened_amount_ld;
    long double opened_limit_ld;
    long long new_amount_ld;
    long double new_balance_ld;
    bool canceled = false;
    
    /*   1. update OPENED_ORDER, set amount to 0   */
    sql = "SELECT SYM, AMOUNT, PRICE FROM OPENED_ORDER WHERE ACCOUNT_ID = " +
          W.quote(account_id) + " AND ORDER_ID = " + W.quote(order_id) + ";";
    R = W.exec(sql);
    res = R.begin();
    if (res == R.end()) { // order does not exist
      response += "  <error id=\"" + order_id +
                  "\">Order does not exist</error>\n";
      return;
    }
    sym = res[0].as<std::string>();
    opened_amount_ld = res[1].as<long long>();
    opened_limit_ld = res[2].as<long double>();
    
    if (opened_amount_ld == 0) {
      response += "  <error id=\"" + order_id +
                  "\">Order is complete, nothing to cancel</error>\n";
      return;
    }
    else if (opened_amount_ld < 0) { // canceling a SELL order, refund shares
      // clear amount to indicate that the order is canceled
      sql = "UPDATE OPENED_ORDER SET AMOUNT = 0 WHERE ACCOUNT_ID = " + 
            W.quote(account_id) + " AND ORDER_ID = " + W.quote(order_id) + ";";
      W.exec(sql);
      
      // add canceled shares to seller's account
      sql = "SELECT \"" + sym + "\" FROM ACCOUNT " +
            "WHERE ACCOUNT_ID = " + W.quote(account_id) + ";";
      R = W.exec(sql);
      /*   TODO: pay attention to possible segfault   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "4" << std::endl;
      }
      /*   TODO: pay attention to negative values   */
      new_amount_ld = res[0].as<long long>() - opened_amount_ld;
      sql = "UPDATE ACCOUNT SET \"" + sym + "\" = " +
            W.quote(std::to_string(new_amount_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(account_id) + ";";
      W.exec(sql);
    }
    else { // canceling a BUY order, refund amount * limit
      // clear amount to indicate that the order is canceled
      sql = "UPDATE OPENED_ORDER SET AMOUNT = 0 WHERE ACCOUNT_ID = " +
            W.quote(account_id) + " AND ORDER_ID = " + W.quote(order_id) + ";";
      W.exec(sql);
      // add canceled amount * limit to buyer's account
      sql = "SELECT BALANCE FROM ACCOUNT WHERE ACCOUNT_ID = " +
            W.quote(account_id) + ";";
      R = W.exec(sql);
      /*   TODO: pay attention to possible segfault   */
      res = R.begin();
      if (res == R.end()) {
        std::cerr << "5" << std::endl;
      }
      new_balance_ld = res[0].as<long double>() + (opened_amount_ld * opened_limit_ld);
      /*   TODO: double check   */
      sql = "UPDATE ACCOUNT SET BALANCE = " + W.quote(set_precision(new_balance_ld)) +
            " WHERE ACCOUNT_ID = " + W.quote(account_id) + ";";
      W.exec(sql);
    }
    
    
    
    /*   2. update CLOSED_ORDER, add cancel info   */
    time_t curr_time = time(NULL);
    sql = "INSERT INTO CLOSED_ORDER " \
          "(ACCOUNT_ID, ORDER_ID, STATUS, SHARES, PRICE, TIME) VALUES (" +
          W.quote(account_id) + ", " +
          W.quote(order_id) + ", 1, " + // order is canceled (0 is executed)
          W.quote(std::to_string(opened_amount_ld)) + ", " +
          W.quote(set_precision(opened_limit_ld)) + ", " +
          W.quote(std::to_string(curr_time)) + ");";
    W.exec(sql);
    
    // get all executed records identified by account_id and order_id
    response += "  <canceled id=\"" + order_id + "\">\n";
    sql = "SELECT * FROM CLOSED_ORDER WHERE ACCOUNT_ID = " + W.quote(account_id) +
          " AND ORDER_ID = " + W.quote(order_id) + ";";
    R = W.exec(sql);
    for (res = R.begin(); res != R.end(); ++res) {
      if (res[2].as<int>() == 0) { // executed order
        response += "    <executed shares=\"" + res[3].as<std::string>() +
                    "\" price=\"" + res[4].as<std::string>() +
                    "\" time=\"" + res[5].as<std::string>() + "\"/>\n";
      }
      else { // canceled order
        response += "    <canceled shares=\"" + res[3].as<std::string>() +
                    "\" time=\"" + res[5].as<std::string>() + "\"/>\n";
        break; // ought to be the last one
      }
    }
    response += "  </canceled>\n";
    
    W.commit();
  }
  catch (std::exception& e) {
    std::cerr << "cancel_order: " << e.what() << std::endl;
    response += "  <error id=\"" + order_id +
                "\">Unable to cancel order</error>\n";
    return;
  }
}






/*   if root node of XML is <transaction>   */
int handle_transactions (xmlpp::TextReader& reader, std::string& response) {
  try {
    std::string account_id;
    std::string sym;
    std::string amount;
    std::string limit;
    std::string order_id;
    std::vector <std::string> id_arr;
    std::vector <std::string> num_shares_arr;
    std::unique_ptr <connection> C(nullptr); // use unique_ptr to prevent memory leak
    bool trans_open = false;
    
    // connect to the database
    // exchange_db is the host name used between containers
#if DOCKER
    C = std::unique_ptr <connection> (new connection("dbname=exchange user=postgres " \
                                                     "password=psql " \
                                                     "host=exchange_db port=5432"));
#else
    C = std::unique_ptr <connection> (new connection("dbname=exchange user=postgres " \
                                                     "password=psql "));
#endif
    
    do {
      if (reader.get_name() == "transactions") {
        if (trans_open == true) {
          break; // encounter </transaction>, finish
        }
        // check if the account exists
        if (reader.get_attribute_count() == 1) { // if node has 1 attribute
          reader.move_to_first_attribute();
          if (reader.get_name() == "account") { // attribute should be "account"
            account_id = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
        }
        else {
          return -1; // invalid XML request
        }
        work W(*(C.get()));
        std::string sql = "SELECT COUNT(ACCOUNT_ID) FROM ACCOUNT " \
                          "WHERE ACCOUNT_ID = " + W.quote(account_id) + ";";
        result R = W.exec(sql);
        result::const_iterator res = R.begin();
        if (res[0].as<int>() == 0) { // account does not exist
          return -3;
        }
        trans_open = true;
      }
      
      
      
      /*   place order   */
      else if (reader.get_name() == "order") {
        if (reader.get_attribute_count() == 3) { // if node has 3 attribute
          reader.move_to_first_attribute();
          if (reader.get_name() == "sym") { // 1st attribute should be "sym"
            sym = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          
          reader.move_to_next_attribute();
          if (reader.get_name() == "amount") { // 2nd attribute should be "amount"
            amount = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          
          reader.move_to_next_attribute();
          if (reader.get_name() == "limit") { // 3rd attribute should be "limit"
            limit = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          // place order
          place_order(C.get(), account_id, sym, amount, limit, response);
        }
        else { // not 2 attributes, invalid XML request
          return -1;
        }
      }
      
      
      
      /*   cancel order   */
      else if (reader.get_name() == "cancel") {
        if (reader.get_attribute_count() == 1) { // if node has 1 attribute
          reader.move_to_first_attribute();
          if (reader.get_name() == "id") { // attribute should be "id"
            order_id = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          // cancel order
          cancel_order(C.get(), account_id, order_id, response);
        }
        else { // not 2 attributes, invalid XML request
          return -1;
        }
      }
      
      
      
      /*   query order   */
      else if (reader.get_name() == "query") {
        if (reader.get_attribute_count() == 1) { // if node has 1 attribute
          reader.move_to_first_attribute(); 
          if (reader.get_name() == "id") { // attribute should be "id"
            order_id = reader.get_value();
          }
          else {
            return -1; // invalid XML request
          }
          // query order
          query_order(C.get(), account_id, order_id, response);
        }
        else { // not 2 attributes, invalid XML request
          return -1;
	}
      }
      else if (reader.get_name() == "#text") {
        continue; // ignore spaces
      }
      else {
        return -1; // invalid XML request
      }
    } while (reader.read()); // read nodes
    C->disconnect();
  }
  catch (std::exception& e) {
    std::cerr << "handle_transaction: " << e.what() << std::endl;
    return -2; // unexpected exception
  }
  
  return 0;
}




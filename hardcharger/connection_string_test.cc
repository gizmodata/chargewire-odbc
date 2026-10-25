// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include <gtest/gtest.h>
#include <odbcabstraction/odbc_impl/ODBCConnection.h>
#include <odbcabstraction/exceptions.h>
using driver::odbcabstraction::Connection;
using driver::odbcabstraction::DriverException;
TEST(ConnectionString, MultipleBracedValuesAndEscapes) {
  Connection::ConnPropertyMap p;
  ODBC::ODBCConnection::getPropertiesFromConnString("Driver={Hardcharger ODBC Driver};pwd={a;b}}c};uid={test};port=31337;queryTag={{\"a\":1}}};", p);
  EXPECT_EQ(p.at("pwd"), "a;b}c"); EXPECT_EQ(p.at("uid"), "test");
  EXPECT_EQ(p.at("port"), "31337"); EXPECT_EQ(p.at("queryTag"), "{\"a\":1}");
}
TEST(ConnectionString, EmptyValuesAndWhitespace) {
  Connection::ConnPropertyMap p;
  ODBC::ODBCConnection::getPropertiesFromConnString("uid=first; uid = last ;pwd=;token={};", p);
  EXPECT_EQ(p.at("uid"), "last"); EXPECT_EQ(p.at("pwd"), ""); EXPECT_EQ(p.at("token"), "");
}
TEST(ConnectionString, MalformedValuesAreRefused) {
  for (const auto& text : {"pwd={no-end", "uid={a}garbage;", "uid;pwd=value", "=value"}) {
    Connection::ConnPropertyMap p;
    EXPECT_THROW(ODBC::ODBCConnection::getPropertiesFromConnString(text, p), DriverException);
  }
}

/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2015 we-do-IT
 *
 *****************************************************************************/

#include "sqlserver_featureset.hpp"
#include "sqlserver_datasource.hpp"
#include "sqlserver_geometry_parser.hpp"

// mapnik
#include <mapnik/global.hpp>
#include <mapnik/debug.hpp>
#include <mapnik/box2d.hpp>
#include <mapnik/geometry.hpp>
#include <mapnik/feature.hpp>
#include <mapnik/feature_layer_desc.hpp>
#include <mapnik/wkb.hpp>
#include <mapnik/unicode.hpp>
#include <mapnik/feature_factory.hpp>

// sql server (odbc)
#include <sqlext.h>
#include <msodbcsql.h>

using mapnik::query;
using mapnik::box2d;
using mapnik::feature_ptr;
using mapnik::geometry_type;
using mapnik::geometry_utils;
using mapnik::transcoder;
using mapnik::feature_factory;
using mapnik::attribute_descriptor;

sqlserver_featureset::sqlserver_featureset(SQLHDBC hdbc,
                                 std::string const& sqlstring,
                                 mapnik::layer_descriptor const& desc
                                 )
    : hstmt_(0),
      desc_(desc),
      tr_(new transcoder(desc.get_encoding())),
      feature_id_(1)
{
    SQLRETURN retcode;
    
    // allocate statement handle
    retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt_);
    if (!SQL_SUCCEEDED(retcode)) {
        throw sqlserver_datasource_exception("could not allocate statement", SQL_HANDLE_DBC, hdbc);
    }
    
    // execute statement
    retcode = SQLExecDirect(hstmt_, (SQLCHAR*)sqlstring.c_str(), SQL_NTS);
    if (!SQL_SUCCEEDED(retcode)) {
        throw sqlserver_datasource_exception("could not execute statement", SQL_HANDLE_STMT, hstmt_);
    }
    
    std::vector<attribute_descriptor>::const_iterator itr = desc_.get_descriptors().begin();
    std::vector<attribute_descriptor>::const_iterator end = desc_.get_descriptors().end();
    ctx_ = boost::make_shared<mapnik::context_type>();
    while (itr != end) {
        ctx_->push(itr->get_name());
        ++itr;
    }
    

}

sqlserver_featureset::~sqlserver_featureset() {
    if (hstmt_) {
        (void)SQLFreeStmt(hstmt_, SQL_CLOSE);
        hstmt_ = 0;
    }
}

feature_ptr sqlserver_featureset::next()
{
    SQLRETURN retcode;
    
    // fetch next result
    retcode = SQLFetch(hstmt_);
    if (retcode == SQL_NO_DATA) {
        // normal end of recordset
        return feature_ptr();
    }
    if (!SQL_SUCCEEDED(retcode)) {
        throw sqlserver_datasource_exception("could not fetch result", SQL_HANDLE_STMT, hstmt_);
    }
   
    // create an empty feature with the next id
    feature_ptr feature(feature_factory::create(ctx_, feature_id_));

    // populate feature geometry and attributes from this row
    std::vector<attribute_descriptor>::const_iterator itr = desc_.get_descriptors().begin();
    std::vector<attribute_descriptor>::const_iterator end = desc_.get_descriptors().end();
    SQLUSMALLINT ColumnNum=1;
    while (itr != end) {
        SQLCHAR sval[2048];
        long ival;
        double dval;
        //SQLCHAR BinaryPtr[2048];    // TODO: handle larger
        SQLCHAR* BinaryPtr = NULL;    // Allocate dynamically
        SQLLEN BinaryLenOrInd;
        SQLLEN LenOrInd;
        switch (itr->get_type()) {
            case mapnik::sqlserver::String:
                retcode = SQLGetData(hstmt_, ColumnNum, SQL_C_CHAR, sval, sizeof(sval), &LenOrInd);
                if (!SQL_SUCCEEDED(retcode)) {
                    throw sqlserver_datasource_exception("could not get string data", SQL_HANDLE_STMT, hstmt_);
                }
                feature->put(itr->get_name(), (UnicodeString)tr_->transcode((char*)sval));
                break;
                
            case mapnik::sqlserver::Integer:
                retcode = SQLGetData(hstmt_, ColumnNum, SQL_C_SLONG, &ival, sizeof(ival), &LenOrInd);
                if (!SQL_SUCCEEDED(retcode)) {
                    throw sqlserver_datasource_exception("could not get int data", SQL_HANDLE_STMT, hstmt_);
                }
                feature->put(itr->get_name(), static_cast<mapnik::value_integer>(ival));
                break;
                
            case mapnik::sqlserver::Double:
                retcode = SQLGetData(hstmt_, ColumnNum, SQL_C_DOUBLE, &dval, sizeof(dval), &LenOrInd);
                if (!SQL_SUCCEEDED(retcode)) {
                    throw sqlserver_datasource_exception("could not get double data", SQL_HANDLE_STMT, hstmt_);
                }
                feature->put(itr->get_name(), dval);
                break;
    
            case mapnik::sqlserver::Geometry:
            case mapnik::sqlserver::Geography: {
                // Call SQLGetData to determine the amount of data that's waiting.
                if (SQLGetData(hstmt_, ColumnNum, SQL_C_BINARY, BinaryPtr, 0, &BinaryLenOrInd) == SQL_SUCCESS_WITH_INFO)
                {
                    BinaryPtr = new SQLCHAR[BinaryLenOrInd];    // should this be SQL_C_BINARY??
                    MAPNIK_LOG_ERROR(sqlserver) << "Allocated required buffer size: " << BinaryLenOrInd;
                    retcode = SQLGetData(hstmt_, ColumnNum, SQL_C_BINARY, BinaryPtr, BinaryLenOrInd, &BinaryLenOrInd);
                    if (!SQL_SUCCEEDED(retcode)) {
                        throw sqlserver_datasource_exception("could not get geometry data into buffer", SQL_HANDLE_STMT, hstmt_);
                    }

                    try
                    {
                        sqlserver_geometry_parser geometry_parser((itr->get_type() == mapnik::sqlserver::Geometry ? Geometry : Geography));
                        mapnik::geometry_container *geom = geometry_parser.parse(BinaryPtr, BinaryLenOrInd);
                        for (size_t j=0; j<geom->size(); j++) {
                            feature->add_geometry(&geom->at(j));
                        }
                    }
                    catch (mapnik::datasource_exception e)
                    {
                        // Cleanup and throw the caught exception
                        delete [] BinaryPtr;
                        BinaryPtr = NULL;
                        throw;
                    }

                    // Cleanup
                    delete [] BinaryPtr;
                    BinaryPtr = NULL;
                }
                else
                {
                    throw sqlserver_datasource_exception("could not get geometry data - failed to get buffer length", SQL_HANDLE_STMT, hstmt_);
                }
                break;
            }

            default:
                MAPNIK_LOG_WARN(sqlserver) << "sqlserver_datasource: unknown/unsupported datatype in column: " << itr->get_name() << " (" << itr->get_type() << ")";
                break;
        }
        ++ColumnNum;
        ++itr;
    }
    ++feature_id_;
    
    return feature;
}

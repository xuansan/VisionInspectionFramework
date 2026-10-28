#pragma once
#include "socket.hpp"
#include <libpq-fe.h>
namespace vision::database {
class Postgres {
public:
    Postgres(const Config& c,Deadline& deadline):deadline_(deadline) {
        const auto port=std::to_string(c.port);
        const char* keys[]={"host","port","dbname","user","password","sslmode","sslrootcert","client_encoding","application_name",nullptr};
        const char* values[]={c.host.c_str(),port.c_str(),c.database.c_str(),c.user.c_str(),c.password.c_str(),
            c.tls=="verify-full"?"verify-full":"disable",c.ca.empty()?nullptr:c.ca.c_str(),"UTF8","vision-db-output",nullptr};
        connection_.reset(PQconnectStartParams(keys,values,0));
        if(!connection_)throw Error("DB.CONNECTION");
        for(;;) {
            deadline_.check();const auto status=PQconnectPoll(connection_.get());
            if(status==PGRES_POLLING_OK)break;
            if(status==PGRES_POLLING_FAILED)throw Error("DB.CONNECTION");
            socket_wait(static_cast<SOCKET>(PQsocket(connection_.get())),status==PGRES_POLLING_READING,
                status==PGRES_POLLING_WRITING,false,deadline_);
        }
        if(PQsetnonblocking(connection_.get(),1)!=0)throw Error("DB.CONNECTION");
        query("SET statement_timeout=1000");
        query("SET lock_timeout=500");
        query("SET search_path=public,pg_catalog");
    }
    Rows query(const std::string& sql,const std::vector<std::string>& params={}) {
        deadline_.check();
        std::vector<const char*> values;for(const auto& p:params)values.push_back(p.c_str());
        if(!PQsendQueryParams(connection_.get(),sql.c_str(),static_cast<int>(values.size()),nullptr,values.data(),nullptr,nullptr,0))
            throw Error("DB.CONNECTION");
        for(;;) {
            deadline_.check();const auto pending=PQflush(connection_.get());
            if(pending<0)throw Error("DB.CONNECTION");if(!pending)break;
            socket_wait(static_cast<SOCKET>(PQsocket(connection_.get())),false,true,false,deadline_);
        }
        Rows rows;bool got=false;
        for(;;) {
            while(PQisBusy(connection_.get())) {
                socket_wait(static_cast<SOCKET>(PQsocket(connection_.get())),true,false,false,deadline_);
                if(!PQconsumeInput(connection_.get()))throw Error("DB.CONNECTION");
            }
            deadline_.check();
            std::unique_ptr<PGresult,decltype(&PQclear)> result(PQgetResult(connection_.get()),PQclear);
            if(!result)break;got=true;
            const auto status=PQresultStatus(result.get());
            if(status!=PGRES_TUPLES_OK&&status!=PGRES_COMMAND_OK) {
                const char* state=PQresultErrorField(result.get(),PG_DIAG_SQLSTATE);
                const std::string_view code=state?state:"";
                if(code=="42501")throw Error("DB.PERMISSION");
                if(code=="42P01"||code=="42703")throw Error("DB.SCHEMA");
                if(code=="23505")throw Error("DB.IDENTITY_CONFLICT");
                if(code=="57014"||code=="55P03")throw Error("DB.TIMEOUT");
                throw Error("DB.SQL");
            }
            const int count=PQntuples(result.get()),fields=PQnfields(result.get());
            if(count>8||fields>16)throw Error("DB.RESULT_LIMIT");
            for(int n=0;n<count;++n) {
                std::vector<std::string> row;
                for(int field=0;field<fields;++field) {
                    if(PQgetisnull(result.get(),n,field)||PQgetlength(result.get(),n,field)>65536)throw Error("DB.RESULT_LIMIT");
                    row.emplace_back(PQgetvalue(result.get(),n,field),static_cast<std::size_t>(PQgetlength(result.get(),n,field)));
                }
                rows.values.push_back(std::move(row));
            }
            const char* affected=PQcmdTuples(result.get());
            if(affected&&*affected)rows.affected=std::stoull(affected);
        }
        if(!got)throw Error("DB.CONNECTION");
        return rows;
    }
    std::string param(unsigned n) const {return "$"+std::to_string(n);}
    std::string insert_suffix() const {return " ON CONFLICT(event_id) DO NOTHING RETURNING event_id";}
    bool inserted(const Rows& rows) const {return rows.values.size()==1;}
private:
    Deadline& deadline_;
    std::unique_ptr<PGconn,decltype(&PQfinish)> connection_{nullptr,PQfinish};
};
}

#pragma once
#include "socket.hpp"
#include <mysql.h>
#include <array>
namespace vision::database {
class Mysql {
public:
    static void initialize_library() {
        static std::once_flag once;
        std::call_once(once,[]{if(mysql_library_init(0,nullptr,nullptr))throw Error("DB.CLIENT_INIT");});
    }
    Mysql(const Config& config,Deadline& deadline):deadline_(deadline) {
        initialize_library();
        if(mysql_thread_init())throw Error("DB.CLIENT_INIT");
        thread_initialized_=true;
        try {
            connection_=mysql_init(nullptr);if(!connection_)throw Error("DB.CLIENT_INIT");
            unsigned seconds=1,protocol=MYSQL_PROTOCOL_TCP;my_bool off=0,on=1;
            auto option=[&](mysql_option key,const void* value){if(mysql_options(connection_,key,value))throw Error("DB.CLIENT_CONFIG");};
            option(MYSQL_OPT_NONBLOCK,nullptr);option(MYSQL_OPT_CONNECT_TIMEOUT,&seconds);
            option(MYSQL_OPT_READ_TIMEOUT,&seconds);option(MYSQL_OPT_WRITE_TIMEOUT,&seconds);
            option(MYSQL_OPT_PROTOCOL,&protocol);option(MYSQL_OPT_RECONNECT,&off);
            option(MYSQL_SET_CHARSET_NAME,"utf8mb4");
            option(MYSQL_OPT_SSL_ENFORCE,config.tls=="verify-full"?&on:&off);
            option(MYSQL_OPT_SSL_VERIFY_SERVER_CERT,config.tls=="verify-full"?&on:&off);
            if(config.tls=="verify-full")option(MYSQL_OPT_SSL_CA,config.ca.c_str());
            MYSQL* connected=nullptr;
            auto status=mysql_real_connect_start(&connected,connection_,config.host.c_str(),config.user.c_str(),
                config.password.c_str(),config.database.c_str(),config.port,nullptr,0);
            while(status)status=mysql_real_connect_cont(&connected,connection_,wait(status));
            if(!connected)throw Error("DB.CONNECTION."+std::to_string(mysql_errno(connection_)));
            if(config.tls=="verify-full"&&!mysql_get_ssl_cipher(connection_))throw Error("DB.TLS_REQUIRED");
            query("SET SESSION innodb_lock_wait_timeout=1");
            query("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
        }catch(...){close();throw;}
    }
    ~Mysql(){close();}
    Mysql(const Mysql&)=delete;Mysql& operator=(const Mysql&)=delete;
    Rows query(const std::string& sql,const std::vector<std::string>& params={}) {
        deadline_.check();
        if(params.empty()&&(sql.starts_with("SET ")||sql.starts_with("CREATE ")||sql.starts_with("ALTER ")||
            sql.starts_with("DROP ")||sql=="BEGIN"||sql=="COMMIT"||sql=="ROLLBACK")) {
            int result=0;
            auto status=mysql_real_query_start(&result,connection_,sql.c_str(),static_cast<unsigned long>(sql.size()));
            while(status)status=mysql_real_query_cont(&result,connection_,wait(status));
            if(result)fail(mysql_errno(connection_));
            return {};
        }
        MYSQL_STMT* statement=mysql_stmt_init(connection_);
        if(!statement)throw Error("DB.CLIENT_INIT");
        struct Statement {
            MYSQL_STMT* value;Mysql& owner;int exceptions=std::uncaught_exceptions();
            ~Statement(){if(std::uncaught_exceptions()>exceptions)owner.abort_socket();mysql_stmt_close(value);}
        } guard{statement,*this};
        int result=0;
        int status=mysql_stmt_prepare_start(&result,statement,sql.c_str(),static_cast<unsigned long>(sql.size()));
        while(status)status=mysql_stmt_prepare_cont(&result,statement,wait(status));
        if(result)fail(mysql_stmt_errno(statement));
        if(mysql_stmt_param_count(statement)!=params.size())throw Error("DB.PARAMETERS");
        std::vector<MYSQL_BIND> bindings(params.size());
        std::vector<unsigned long> lengths(params.size());
        for(std::size_t n=0;n<params.size();++n) {
            if(params[n].size()>65536||params[n].find('\0')!=std::string::npos)throw Error("DB.PARAMETERS");
            lengths[n]=static_cast<unsigned long>(params[n].size());
            bindings[n].buffer_type=MYSQL_TYPE_STRING;bindings[n].buffer=const_cast<char*>(params[n].data());
            bindings[n].buffer_length=lengths[n];bindings[n].length=&lengths[n];
        }
        if(!params.empty()&&mysql_stmt_bind_param(statement,bindings.data()))throw Error("DB.PARAMETERS");
        status=mysql_stmt_execute_start(&result,statement);
        while(status)status=mysql_stmt_execute_cont(&result,statement,wait(status));
        if(result)fail(mysql_stmt_errno(statement));
        Rows rows;const unsigned fields=mysql_stmt_field_count(statement);
        if(fields>16)throw Error("DB.RESULT_LIMIT");
        if(fields) {
            status=mysql_stmt_store_result_start(&result,statement);
            while(status)status=mysql_stmt_store_result_cont(&result,statement,wait(status));
            if(result)fail(mysql_stmt_errno(statement));
            if(mysql_stmt_num_rows(statement)>8)throw Error("DB.RESULT_LIMIT");
            std::vector<std::array<char,65537>> cells(fields);
            std::vector<MYSQL_BIND> outputs(fields);std::vector<unsigned long> sizes(fields);
            std::vector<my_bool> nulls(fields),errors(fields);
            for(unsigned n=0;n<fields;++n) {
                outputs[n].buffer_type=MYSQL_TYPE_STRING;outputs[n].buffer=cells[n].data();outputs[n].buffer_length=65536;
                outputs[n].length=&sizes[n];outputs[n].is_null=&nulls[n];outputs[n].error=&errors[n];
            }
            if(mysql_stmt_bind_result(statement,outputs.data()))throw Error("DB.CLIENT");
            for(;;) {
                deadline_.check();result=mysql_stmt_fetch(statement);
                if(result==MYSQL_NO_DATA)break;if(result)throw Error("DB.RESULT_LIMIT");
                std::vector<std::string> row;
                for(unsigned n=0;n<fields;++n) {
                    if(nulls[n]||errors[n]||sizes[n]>65536)throw Error("DB.RESULT_LIMIT");
                    row.emplace_back(cells[n].data(),sizes[n]);
                }
                rows.values.push_back(std::move(row));
            }
        }
        const auto affected=mysql_stmt_affected_rows(statement);
        if(affected!=static_cast<my_ulonglong>(-1))rows.affected=affected;
        return rows;
    }
    std::string param(unsigned) const {return "?";}
    std::string insert_suffix() const {return " ON DUPLICATE KEY UPDATE event_id=event_id";}
    bool inserted(const Rows& rows) const {return rows.affected==1;}
private:
    [[noreturn]] static void fail(unsigned code) {
        if(code==1142||code==1143||code==1044)throw Error("DB.PERMISSION");
        if(code==1146||code==1054)throw Error("DB.SCHEMA");
        if(code==1062)throw Error("DB.IDENTITY_CONFLICT");
        if(code==1205||code==1213)throw Error("DB.TIMEOUT");
        throw Error("DB.SQL."+std::to_string(code));
    }
    int wait(int status) {
        deadline_.check();
        const auto socket=static_cast<SOCKET>(mysql_get_socket(connection_));
        auto ready=socket_wait(socket,(status&MYSQL_WAIT_READ)!=0,(status&MYSQL_WAIT_WRITE)!=0,(status&MYSQL_WAIT_EXCEPT)!=0,deadline_);
        if(!ready&&(status&MYSQL_WAIT_TIMEOUT)) {
            const auto now=std::chrono::steady_clock::now();
            if(!waiting_) {waiting_=true;timeout_=now+std::chrono::milliseconds(mysql_get_timeout_value_ms(connection_));}
            if(now>=timeout_)ready|=MYSQL_WAIT_TIMEOUT;
        }
        if(ready)waiting_=false;
        return ready;
    }
    void abort_socket() noexcept {
        if(connection_)shutdown(static_cast<SOCKET>(mysql_get_socket(connection_)),SD_BOTH);
    }
    void close() noexcept {
        if(connection_){abort_socket();mysql_close(connection_);connection_=nullptr;}
        if(thread_initialized_){mysql_thread_end();thread_initialized_=false;}
    }
    Deadline& deadline_;MYSQL* connection_{};bool thread_initialized_{},waiting_{};
    std::chrono::steady_clock::time_point timeout_;
};
}

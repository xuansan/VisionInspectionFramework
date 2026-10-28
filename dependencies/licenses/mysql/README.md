# MySQL 输出客户端通知

运行时分发 libmariadb.dll，来自 MariaDB Connector/C 3.4.5，
固定提交及本地补丁见 dependencies/artifacts.lock.json。
客户端采用 LGPL 系列许可；COPYING.LIB 为上游 LGPL 2.1 正文，
源码个别文件保留 LGPL 2-or-later 等原始声明，分发对应源码时须一并保留。

动态部署 OpenSSL 3.0.16 DLL，其 Apache 2.0 正文见 OpenSSL-LICENSE.txt。
Connector 内置 zlib 1.2.13，原始许可保存在 zlib-original-header.h 顶部。
本批关闭 iconv/parsec，不部署额外动态认证插件或 MySQL 服务端。
MySQL Community 服务端为单独 GPL 测试依赖，不是插件运行库。

发布时提供实际客户端对应源码、补丁、构建说明、版权及替换所需材料；
开发目录保留源码不等于已经履行全部分发义务。

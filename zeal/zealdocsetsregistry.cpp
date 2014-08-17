#include <QThread>
#include <QVariant>
#include <QDebug>
#include <QtSql/QSqlQuery>
#include <QStandardPaths>

#include "zealdocsetsregistry.h"
#include "zealsearchresult.h"
#include "zealsearchquery.h"

ZealDocsetsRegistry* ZealDocsetsRegistry::m_Instance;

ZealDocsetsRegistry* docsets = ZealDocsetsRegistry::instance();

void ZealDocsetsRegistry::addDocset(const QString& path) {
    auto dir = QDir(path);
    auto name = dir.dirName().replace(".docset", "");
    if(docs.contains(name)){
        remove(name);
    }
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    docsetEntry entry;

    if(QFile::exists(dir.filePath("index.sqlite"))) {
        db.setDatabaseName(dir.filePath("index.sqlite"));
        db.open();
        entry.type = ZEAL;
    } else {
        auto dashFile = QDir(QDir(dir.filePath("Contents")).filePath("Resources")).filePath("docSet.dsidx");
        db.setDatabaseName(dashFile);
        db.open();
        auto q = db.exec("select name from sqlite_master where type='table'");
        QStringList tables;
        while(q.next()) {
            tables.append(q.value(0).toString());
        }
        if(tables.contains("searchIndex")) {
            entry.type = DASH;
        } else {
            entry.type = ZDASH;
        }
    }
    entry.name = name;
    // TODO: make customizable prefixes.
    entry.prefix = name;
    entry.db = db;
    entry.dir = dir;

    // Read metadata
    ZealDocsetMetadata meta;
    meta.read(path+"/meta.json");
    entry.metadata = meta;
    docs[name] = entry;
}

ZealDocsetsRegistry::ZealDocsetsRegistry() :
    settings("Zeal", "Zeal")
{
    lastQuery = -1;
    auto thread = new QThread(this);
    moveToThread(thread);
    thread->start();
}

bool ZealDocsetsRegistry::hasDocset(const QString &name)
{
    this->docs.contains(name);
}

QList<ZealDocsetsRegistry::docsetEntry> ZealDocsetsRegistry::docsets()
{
    return this->docs.values();
}

void ZealDocsetsRegistry::runQuery(const QString& query)
{
    lastQuery += 1;
    QMetaObject::invokeMethod(this, "_runQuery", Qt::QueuedConnection, Q_ARG(QString, query), Q_ARG(int, lastQuery));
}

void ZealDocsetsRegistry::invalidateQueries()
{
    lastQuery += 1;
}

void ZealDocsetsRegistry::_runQuery(const QString& rawQuery, int queryNum)
{
    if(queryNum != lastQuery) return; // some other queries pending - ignore this one

    QList<ZealSearchResult> results;
    ZealSearchQuery query(rawQuery);

    QString docsetPrefix = query.getDocsetFilter();
    QString preparedQuery = query.getSanitizedQuery();
    bool hasPrefixFilter = !docsetPrefix.isEmpty();

    for (const ZealDocsetsRegistry::docsetEntry docset : docsets()) {
        if (hasPrefixFilter && !docset.prefix.contains(docsetPrefix, Qt::CaseInsensitive)) {
            // Filter out this docset as the names don't match the docset prefix
            continue;
        }

        QString qstr;
        QSqlQuery q;
        QList<QList<QVariant> > found;
        bool withSubStrings = false;
        // %.%1% for long Django docset values like django.utils.http
        // %::%1% for long C++ docset values like std::set
        // %/%1% for long Go docset values like archive/tar
        QString subNames = QString(" or %1 like '%.%2%' escape '\\'");
        subNames += QString(" or %1 like '%::%2%' escape '\\'");
        subNames += QString(" or %1 like '%/%2%' escape '\\'");
        while(found.size() < 100) {
            auto curQuery = preparedQuery;
            QString notQuery; // don't return the same result twice
            QString parentQuery;
            if(withSubStrings) {
                // if less than 100 found starting with query, search all substrings
                curQuery = "%"+preparedQuery;
                // don't return 'starting with' results twice
                if(docset.type == ZDASH) {
                    notQuery = QString(" and not (ztokenname like '%1%' escape '\\' %2) ").arg(preparedQuery, subNames.arg("ztokenname", preparedQuery));
                } else {
                    if(docset.type == ZEAL) {
                        notQuery = QString(" and not (t.name like '%1%' escape '\\') ").arg(preparedQuery);
                        parentQuery = QString(" or t2.name like '%1%' escape '\\' ").arg(preparedQuery);
                    } else { // DASH
                        notQuery = QString(" and not (t.name like '%1%' escape '\\' %2) ").arg(preparedQuery, subNames.arg("t.name", preparedQuery));
                    }
                }
            }
            int cols = 3;
            if(docset.type == ZEAL) {
                qstr = QString("select t.name, t2.name, t.path from things t left join things t2 on t2.id=t.parent where "
                               "(t.name like '%1%' escape '\\'  %3) %2 order by lower(t.name) asc, t.path asc limit 100").arg(curQuery, notQuery, parentQuery);

            } else if(docset.type == DASH) {
                qstr = QString("select t.name, null, t.path from searchIndex t where (t.name "
                               "like '%1%' escape '\\' %3)  %2 order by lower(t.name) asc, t.path asc limit 100").arg(curQuery, notQuery, subNames.arg("t.name", curQuery));
            } else if(docset.type == ZDASH) {
                cols = 4;
                qstr = QString("select ztokenname, null, zpath, zanchor from ztoken "
                                "join ztokenmetainformation on ztoken.zmetainformation = ztokenmetainformation.z_pk "
                                "join zfilepath on ztokenmetainformation.zfile = zfilepath.z_pk where (ztokenname "

                               "like '%1%' escape '\\' %3) %2 order by lower(ztokenname) asc, zpath asc, "
                               "zanchor asc limit 100").arg(curQuery, notQuery, subNames.arg("ztokenname", curQuery));
            }
            q = db(docset.name).exec(qstr);
            while(q.next()) {
                QList<QVariant> values;
                for(int i = 0; i < cols; ++i) {
                    values.append(q.value(i));
                }
                found.append(values);
            }

            if(withSubStrings) break;
            withSubStrings = true;  // try again searching for substrings
        }
        for(auto &row : found) {
            QString parentName;
            if(!row[1].isNull()) {
                parentName = row[1].toString();
            }
            auto path = row[2].toString();
            // FIXME: refactoring to use common code in ZealListModel and ZealDocsetsRegistry
            if(docset.type == DASH || docset.type == ZDASH) {
                path = QDir(QDir(QDir("Contents").filePath("Resources")).filePath("Documents")).filePath(path);
            }
            if(docset.type == ZDASH) {
                path += "#" + row[3].toString();
            }
            auto itemName = row[0].toString();
            QRegExp matchMethodName("^([^\\(]+)(?:\\(.*\\))?$");
            if (matchMethodName.indexIn(itemName) != -1) {
                itemName = matchMethodName.cap(1);
            }
            QString separators[] = {".", "::", "/"};
            for(unsigned i = 0; i < sizeof separators / sizeof *separators; ++i) {
                QString sep = separators[i];
                if(itemName.indexOf(sep) != -1 && itemName.indexOf(sep) != 0 && row[1].isNull()) {
                    auto splitted = itemName.split(sep);
                    itemName = splitted.at(splitted.size()-1);
                    parentName = splitted.at(splitted.size()-2);
                }
            }
            results.append(ZealSearchResult(itemName, parentName, path, docset.name, preparedQuery));
        }
    }
    qSort(results);
    if(queryNum != lastQuery) return; // some other queries pending - ignore this one

    queryResults = results;
    emit queryCompleted();
}

const QList<ZealSearchResult>& ZealDocsetsRegistry::getQueryResults()
{
    return queryResults;
}

QString ZealDocsetsRegistry::docsetsDir(){
    if(settings.contains("docsetsDir")) {
        return settings.value("docsetsDir").toString();
    } else {
        auto dataLocation = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
        auto dataDir = QDir(dataLocation);
        if(!dataDir.cd("docsets")) {
            dataDir.mkpath("docsets");
        }
        dataDir.cd("docsets");
        return dataDir.absolutePath();
    }
}

void ZealDocsetsRegistry::initialiseDocsets()
{
    clear();
    QDir dataDir( docsetsDir() );
    for(auto subdir : dataDir.entryInfoList()) {
        if(subdir.isDir() && subdir.fileName() != "." && subdir.fileName() != "..") {
            QMetaObject::invokeMethod(this, "addDocset", Qt::BlockingQueuedConnection,
                                      Q_ARG(QString, subdir.absoluteFilePath()));
        }
    }
    QDir appDir( QCoreApplication::applicationDirPath() );
    if(appDir.cd("docsets")){
        for(auto subdir : appDir.entryInfoList()) {
            if(subdir.isDir() && subdir.fileName() != "." && subdir.fileName() != "..") {
                QMetaObject::invokeMethod(this, "addDocset", Qt::BlockingQueuedConnection,
                                          Q_ARG(QString, subdir.absoluteFilePath()));
            }
        }
    }
}

// Inventatory - local manufacturer parametric catalogue storage and lookup.
#include "core/CatalogueDatabase.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace inventatory {
using namespace std;
namespace {

string lower(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return value;
}
string text(sqlite3_stmt* s, int column) {
  const auto* value = sqlite3_column_text(s, column);
  return value ? reinterpret_cast<const char*>(value) : "";
}
bool exec(sqlite3* db, const string& sql) { return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; }
struct Db { sqlite3* value = nullptr; ~Db() { if (value) sqlite3_close(value); } };
struct Statement { sqlite3_stmt* value = nullptr; ~Statement() { if (value) sqlite3_finalize(value); } };

const vector<CatalogueProfile> kProfiles = {
  {"murata-capacitors-v1","1","Murata","Capacitors","",{"murata"},{"Part Number","Product ID","MPN"},{"Series"},{"Alternate Part Number"},{"Case Code","Package"},{"Series"},{"Description"},{"Status"},{{{"Capacitance"},"capacitance","F",""},{{"Rated Voltage","Voltage"},"rated_voltage","V",""},{{"Tolerance"},"tolerance","%",""},{{"ESR"},"esr","Ohm",""},{{"Temperature Characteristic","Dielectric"},"dielectric","",""}},false},
  {"tdk-mlcc-v1","1","TDK","MLCC","",{"tdk","mlcc"},{"Part Number","Ordering Code","MPN"},{"Series"},{"Alternative"},{"Case Size","Package"},{"Series"},{"Description"},{"Status"},{{{"Capacitance"},"capacitance","F",""},{{"Rated Voltage"},"rated_voltage","V",""},{{"Tolerance"},"tolerance","%",""},{{"Temperature Characteristic"},"dielectric","",""},{{"Dissipation Factor"},"dissipation_factor","%",""},{{"Insulation Resistance"},"insulation_resistance","Ohm",""},{{"AEC-Q200"},"aec_q200","",""}},false},
  {"kemet-yageo-mlcc-v1","1","KEMET / Yageo","Capacitors","",{"kemet","yageo"},{"Part Number","MPN"},{"Series"},{"Alias"},{"Case","Package"},{"Series"},{"Description"},{"Status"},{{{"Capacitance"},"capacitance","F",""},{{"Voltage"},"rated_voltage","V",""},{{"Tolerance"},"tolerance","%",""},{{"Dielectric"},"dielectric","",""},{{"MSL"},"msl","",""},{{"AEC"},"aec_qualification","",""}},false},
  {"vishay-current-sense-v1","1","Vishay","Current-sense resistors","",{"vishay"},{"Part Number","MPN"},{},{},{"Case","Package"},{"Series"},{"Description"},{"Status"},{{{"Resistance"},"resistance","Ohm",""},{{"Tolerance"},"tolerance","%",""},{{"Power"},"power","W",""},{{"TCR"},"temperature_coefficient","ppm/degC",""}},false},
  {"nexperia-discretes-v1","1","Nexperia","Diodes, BJTs and MOSFETs","",{"nexperia"},{"Type number","Orderable part number","MPN"},{"Type number"},{"Orderable part number"},{"Package"},{"Family"},{"Description"},{"Status"},{{{"VR","VCEO","VDS"},"voltage_rating","V",""},{{"IF","IC","ID"},"current_rating","A",""},{{"VF"},"forward_voltage","V",""},{{"RDS(on)"},"rds_on","Ohm",""},{{"Automotive"},"automotive_qualification","",""}},false},
  {"ti-parametric-v1","1","Texas Instruments","Amplifiers, MOSFETs and timers","",{"ti_","texas","opamp","mosfet","timer"},{"Orderable Part Number","Part Number","MPN"},{"Generic Part Number","Device"},{"Orderable Part Number"},{"Package Group","Package"},{"Family"},{"Description"},{"Status"},{{{"Channels"},"channel_count","",""},{{"Supply voltage (min)"},"supply_voltage_min","V","min"},{{"Supply voltage (max)"},"supply_voltage_max","V","max"},{{"Offset voltage"},"offset_voltage","V",""},{{"GBW","Bandwidth"},"bandwidth","Hz",""},{{"Slew rate"},"slew_rate","V/us",""},{{"VDS"},"vds","V",""},{{"RDS(on)"},"rds_on","Ohm",""}},false},
  {"adi-amplifiers-v1","1","Analog Devices","Precision amplifiers","",{"analogdevices","analog_devices","adi_"},{"Orderable Part Number","Model","MPN"},{"Model"},{"Orderable Part Number"},{"Package"},{"Product Family"},{"Description"},{"Status"},{{{"Channels"},"channel_count","",""},{{"Supply Voltage Min"},"supply_voltage_min","V","min"},{{"Supply Voltage Max"},"supply_voltage_max","V","max"},{{"Offset Voltage"},"offset_voltage","V",""},{{"Input Bias Current"},"input_bias_current","A",""},{{"Bandwidth"},"bandwidth","Hz",""}},false},
  {"microchip-parametric-v1","1","Microchip","MCUs and amplifiers","",{"microchip"},{"Part Number","Device","MPN"},{"Device"},{"Orderable Part Number"},{"Package"},{"Family"},{"Description"},{"Status"},{{{"Program Memory"},"program_memory","B",""},{{"RAM"},"ram","B",""},{{"EEPROM"},"eeprom","B",""},{{"Pin Count"},"pin_count","",""},{{"Operating Voltage"},"operating_voltage","V",""},{{"Max Clock"},"clock","Hz",""},{{"ADC Resolution"},"adc_resolution","bit",""}},false}
};

vector<string> parseCsvRow(const string& line, char delimiter, bool& malformed) {
  vector<string> row; string value; bool quoted = false; malformed = false;
  for (size_t i=0;i<line.size();++i) { char c=line[i]; if (c=='"') { if (quoted && i+1<line.size() && line[i+1]=='"') { value+='"'; ++i; } else quoted=!quoted; } else if (c==delimiter && !quoted) { row.push_back(value); value.clear(); } else if (c!='\r') value+=c; }
  row.push_back(value); malformed=quoted; return row;
}
string uniqueHeader(string h, map<string,int>& seen) { h=trim(h); int& n=seen[h]; ++n; return n==1?h:h+" ["+to_string(n)+"]"; }
int findColumn(const vector<string>& headers, const vector<string>& candidates) { for (const auto& c:candidates) for(size_t i=0;i<headers.size();++i) if(lower(trim(headers[i]))==lower(trim(c))) return static_cast<int>(i); return -1; }
string cell(const vector<string>& row,int i) { return i>=0 && static_cast<size_t>(i)<row.size()?trim(row[i]):""; }
string hashFile(const filesystem::path& p) { // Stable local identity; not a security primitive.
  ifstream in(p,ios::binary); uint64_t h=1469598103934665603ULL; char b[8192]; while(in.read(b,sizeof b)||in.gcount()) for(streamsize i=0;i<in.gcount();++i){h^=static_cast<unsigned char>(b[i]);h*=1099511628211ULL;} ostringstream out; out<<hex<<setfill('0')<<setw(16)<<h; return out.str();
}
string schema() { return R"SQL(
PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS catalogue_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
INSERT OR IGNORE INTO catalogue_metadata VALUES('schema_version','1');
CREATE TABLE IF NOT EXISTS snapshots(id INTEGER PRIMARY KEY,source_id TEXT NOT NULL,manufacturer TEXT NOT NULL,profile_id TEXT NOT NULL,profile_version TEXT NOT NULL,filename TEXT NOT NULL,file_hash TEXT NOT NULL UNIQUE,imported_at INTEGER NOT NULL,rows_count INTEGER NOT NULL DEFAULT 0,parts_count INTEGER NOT NULL DEFAULT 0,aliases_count INTEGER NOT NULL DEFAULT 0,properties_count INTEGER NOT NULL DEFAULT 0,warnings_count INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS parts(id INTEGER PRIMARY KEY,source_id TEXT NOT NULL,manufacturer TEXT NOT NULL,manufacturer_key TEXT NOT NULL,exact_mpn TEXT,mpn_key TEXT,base_part TEXT,package_variant TEXT,packaging_variant TEXT,series TEXT,category TEXT,description TEXT,status TEXT,snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,UNIQUE(source_id,mpn_key,snapshot_id));
CREATE INDEX IF NOT EXISTS idx_parts_exact ON parts(manufacturer_key,mpn_key);
CREATE TABLE IF NOT EXISTS aliases(id INTEGER PRIMARY KEY,part_id INTEGER NOT NULL REFERENCES parts(id) ON DELETE CASCADE,alias TEXT NOT NULL,alias_key TEXT NOT NULL,kind TEXT NOT NULL,UNIQUE(part_id,alias_key));
CREATE INDEX IF NOT EXISTS idx_alias_exact ON aliases(alias_key);
CREATE TABLE IF NOT EXISTS properties(id INTEGER PRIMARY KEY,part_id INTEGER NOT NULL REFERENCES parts(id) ON DELETE CASCADE,name TEXT NOT NULL,nominal REAL,min_value REAL,typical REAL,max_value REAL,tolerance REAL,unit TEXT,condition TEXT,qualifier TEXT,source_column TEXT NOT NULL,raw_value TEXT,raw_unit TEXT,mapping_status TEXT NOT NULL,profile_id TEXT NOT NULL,profile_version TEXT NOT NULL,snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS import_warnings(id INTEGER PRIMARY KEY,snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,row_number INTEGER,message TEXT NOT NULL);
)SQL"; }
}

const vector<ManufacturerSource>& manufacturerSources() { static const vector<ManufacturerSource> sources={
 {"murata","Murata","Murata", "https://www.murata.com/en-global/search/productsearch",{".csv",".xlsx"},{"Capacitors","MLCC","Polymer capacitors"},DownloadMode::BrowserGuided,false,false,"murata-capacitors-v1","Export the desired parametric table on Murata's official page."},
 {"tdk","TDK","TDK", "https://product.tdk.com/en/search/capacitor/ceramic/mlcc/",{".csv",".xlsx"},{"MLCC"},DownloadMode::BrowserGuided,false,false,"tdk-mlcc-v1","Use TDK's official export control."},
 {"kemet-yageo","KEMET / Yageo","KEMET / Yageo", "https://www.kemet.com/en/us/capacitors.html",{".csv",".xlsx"},{"Capacitors","MLCC"},DownloadMode::BrowserGuided,false,false,"kemet-yageo-mlcc-v1","Export from the official product selector."},
 {"vishay","Vishay","Vishay", "https://www.vishay.com/en/resistors/",{".csv",".xlsx"},{"Current-sense resistors"},DownloadMode::BrowserGuided,false,false,"vishay-current-sense-v1","Series-only exports are retained as series records and never exact matched."},
 {"nexperia","Nexperia","Nexperia", "https://www.nexperia.com/products",{".csv",".xlsx"},{"Diodes","BJTs","MOSFETs"},DownloadMode::BrowserGuided,false,false,"nexperia-discretes-v1","Use the official parametric export."},
 {"ti","Texas Instruments","Texas Instruments", "https://www.ti.com/selection-tools/parametric-search.html",{".csv",".xlsx"},{"Amplifiers","MOSFETs","Timers"},DownloadMode::BrowserGuided,false,false,"ti-parametric-v1","Use TI's Download/Excel control."},
 {"adi","Analog Devices","Analog Devices", "https://www.analog.com/en/parametricsearch.html",{".csv",".xlsx"},{"Precision amplifiers"},DownloadMode::BrowserGuided,false,false,"adi-amplifiers-v1","Use ADI's official export control."},
 {"microchip","Microchip","Microchip", "https://www.microchip.com/en-us/products",{".csv",".xlsx"},{"MCUs","Amplifiers"},DownloadMode::BrowserGuided,false,false,"microchip-parametric-v1","Use Microchip's official export control."}
 }; return sources; }
const vector<CatalogueProfile>& catalogueProfiles(){return kProfiles;}

string normalizeMpn(const string& input) { string out=trim(input); for(char& c:out) if(static_cast<unsigned char>(c)<128)c=static_cast<char>(toupper(static_cast<unsigned char>(c))); return out; }
const CatalogueProfile* detectCatalogueProfile(const filesystem::path& file,const vector<string>& headers){ string n=lower(file.filename().string()); for(const auto& p:kProfiles) for(const auto& pattern:p.filenamePatterns) if(n.find(lower(pattern))!=string::npos)return &p; for(const auto& p:kProfiles) if(findColumn(headers,p.mpnColumns)>=0) return &p; return nullptr; }

EngineeringValue parseEngineeringValue(const string& input,const string& contextUnit) {
  EngineeringValue out; out.rawValue=input; out.originalText=input; out.rawUnit=contextUnit; string s=trim(input); if(s.empty()) return out;
  for(const string micro : {"µ", "μ"}) for(size_t p;(p=s.find(micro))!=string::npos;) s.replace(p,micro.size(),"u");
  for(const string dash : {"–", "—"}) for(size_t p;(p=s.find(dash))!=string::npos;) s.replace(p,dash.size(),"-");
  smatch m; regex range(R"(^\s*([+-]?\d+(?:\.\d+)?)\s*(?:to|-)\s*([+-]?\d+(?:\.\d+)?)\s*(.*)$)",regex::icase);
  regex tolerance(R"(^\s*[+\-±]\s*(\d+(?:\.\d+)?)\s*%\s*$)");
  if(regex_match(s,m,tolerance)){out.tolerance=stod(m[1]);out.unit="%";return out;}
  string number,unit;
  if(regex_match(s,m,range)){out.minimum=stod(m[1]);out.maximum=stod(m[2]);unit=trim(m[3]);}
  else { regex scalar(R"(^\s*([+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*([^\s]*)\s*$)"); if(!regex_match(s,m,scalar)){out.warning="Ambiguous engineering value";return out;} out.nominal=stod(m[1]); unit=trim(m[2]); }
  if(unit.empty()) unit=contextUnit; if(unit=="VDC")unit="V"; if(unit=="Ω")unit="Ohm"; if(unit=="°C")unit="degC";
  static const map<string,double> prefixes={{"p",1e-12},{"n",1e-9},{"u",1e-6},{"m",1e-3},{"k",1e3},{"K",1e3},{"M",1e6},{"G",1e9}};
  string base=unit; double factor=1; for(const auto& [prefix,f]:prefixes) if(unit.size()>prefix.size() && unit.rfind(prefix,0)==0){base=unit.substr(prefix.size());factor=f;break;}
  if(unit.size()==1 && prefixes.count(unit) && !contextUnit.empty()){factor=prefixes.at(unit);base=contextUnit;}
  if(base.empty()){out.warning="Unit is ambiguous without column context";return out;} out.unit=base;
  if(out.nominal)*out.nominal*=factor;if(out.minimum)*out.minimum*=factor;if(out.maximum)*out.maximum*=factor; return out;
}

bool CatalogueMatch::matched() const{return status==CatalogueMatchStatus::ExactMatch||status==CatalogueMatchStatus::AliasMatch;}
bool CatalogueDatabase::open(const filesystem::path& path){path_=path;filesystem::create_directories(path.parent_path());Db db;if(sqlite3_open(path.string().c_str(),&db.value)!=SQLITE_OK)return false;sqlite3_busy_timeout(db.value,5000);available_=exec(db.value,schema());version_="1";return available_;}
bool CatalogueDatabase::available()const{return available_;} const string& CatalogueDatabase::version()const{return version_;}

CatalogueMatch CatalogueDatabase::lookup(const string& manufacturer,const string& mpn)const {
  if(!available_)return {CatalogueMatchStatus::DatabaseUnavailable,{}}; Db db;if(sqlite3_open_v2(path_.string().c_str(),&db.value,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)return {CatalogueMatchStatus::DatabaseUnavailable,{}};
  const string mk=normalizeMpn(manufacturer), pk=normalizeMpn(mpn); if(pk.empty())return {};
  auto run=[&](bool alias)->CatalogueMatch { Statement s; string sql=alias?"SELECT p.id,p.manufacturer,p.exact_mpn,p.base_part,p.description,p.category,p.package_variant,p.packaging_variant,p.series,p.snapshot_id FROM aliases a JOIN parts p ON p.id=a.part_id WHERE a.alias_key=? AND (?='' OR p.manufacturer_key=?) AND p.snapshot_id=(SELECT max(s2.id) FROM snapshots s2 WHERE s2.source_id=p.source_id) ORDER BY p.snapshot_id DESC LIMIT 2":"SELECT id,manufacturer,exact_mpn,base_part,description,category,package_variant,packaging_variant,series,snapshot_id FROM parts WHERE mpn_key=? AND (?='' OR manufacturer_key=?) AND snapshot_id=(SELECT max(s2.id) FROM snapshots s2 WHERE s2.source_id=parts.source_id) ORDER BY snapshot_id DESC LIMIT 2"; sqlite3_prepare_v2(db.value,sql.c_str(),-1,&s.value,nullptr);sqlite3_bind_text(s.value,1,pk.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s.value,2,mk.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s.value,3,mk.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(s.value)!=SQLITE_ROW)return {};CatalogueRecord r;r.componentId=to_string(sqlite3_column_int64(s.value,0));r.manufacturer=text(s.value,1);r.manufacturerPartNumber=text(s.value,2);r.basePart=text(s.value,3);r.canonicalName=text(s.value,4);r.category=text(s.value,5);r.packageVariant=text(s.value,6);r.packagingVariant=text(s.value,7);r.series=text(s.value,8);r.databaseVersion=to_string(sqlite3_column_int64(s.value,9));if(sqlite3_step(s.value)==SQLITE_ROW)return {CatalogueMatchStatus::Ambiguous,{}};return {alias?CatalogueMatchStatus::AliasMatch:CatalogueMatchStatus::ExactMatch,r};};
  auto exact=run(false);return exact.status==CatalogueMatchStatus::NotFound?run(true):exact;
}

CatalogueImportStats CatalogueDatabase::importFile(const filesystem::path& path,const CatalogueProfile* selected,const CatalogueImportOptions& options){CatalogueImportStats stats;if(!available_){stats.error="Catalogue database is unavailable";return stats;}if(lower(path.extension().string())!=".csv"){stats.error="XLSX workbook support is unavailable in this build; choose the manufacturer's CSV export";return stats;}ifstream in(path,ios::binary);if(!in){stats.error="Unable to open catalogue file";return stats;}string first;getline(in,first);if(first.rfind("\xEF\xBB\xBF",0)==0)first.erase(0,3);char delimiter=count(first.begin(),first.end(),';')>count(first.begin(),first.end(),',')?';':',';bool malformed=false;auto headers=parseCsvRow(first,delimiter,malformed);map<string,int> seen;for(auto& h:headers)h=uniqueHeader(h,seen);const auto* profile=selected?selected:detectCatalogueProfile(path,headers);if(!profile){stats.error="No manufacturer profile matches this file";return stats;}stats.profileId=profile->id;int mpnCol=findColumn(headers,profile->mpnColumns);if(mpnCol<0&&!profile->seriesOnly){stats.error="The expected MPN column is missing";return stats;}int baseCol=findColumn(headers,profile->basePartColumns),packageCol=findColumn(headers,profile->packageColumns),seriesCol=findColumn(headers,profile->seriesColumns),descriptionCol=findColumn(headers,profile->descriptionColumns),statusCol=findColumn(headers,profile->statusColumns);vector<pair<int,PropertyMapping>> mappings;for(const auto& m:profile->properties){int c=findColumn(headers,m.columns);if(c>=0)mappings.push_back({c,m});}
  Db db;if(sqlite3_open(path_.string().c_str(),&db.value)!=SQLITE_OK){stats.error="Unable to open catalogue database";return stats;}sqlite3_busy_timeout(db.value,5000);if(!exec(db.value,"BEGIN IMMEDIATE")){stats.error="Catalogue database is busy";return stats;}const string hash=hashFile(path);stats.fileHash=hash;Statement duplicate;sqlite3_prepare_v2(db.value,"SELECT id FROM snapshots WHERE file_hash=?",-1,&duplicate.value,nullptr);sqlite3_bind_text(duplicate.value,1,hash.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(duplicate.value)==SQLITE_ROW){exec(db.value,"ROLLBACK");stats.duplicate=true;stats.snapshotId=sqlite3_column_int64(duplicate.value,0);return stats;}
  Statement snap;sqlite3_prepare_v2(db.value,"INSERT INTO snapshots(source_id,manufacturer,profile_id,profile_version,filename,file_hash,imported_at) VALUES(?,?,?,?,?,?,?)",-1,&snap.value,nullptr);vector<string> sv={profile->id,profile->manufacturer,profile->id,profile->version,path.filename().string(),hash};for(int i=0;i<6;++i)sqlite3_bind_text(snap.value,i+1,sv[i].c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(snap.value,7,time(nullptr));if(sqlite3_step(snap.value)!=SQLITE_DONE){exec(db.value,"ROLLBACK");stats.error="Unable to create catalogue snapshot";return stats;}stats.snapshotId=sqlite3_last_insert_rowid(db.value);
  string line;size_t rowNo=1;while(getline(in,line)){++rowNo;if(options.cancel&&options.cancel->load()){stats.cancelled=true;break;}if(trim(line).empty())continue;++stats.rows;bool bad=false;auto row=parseCsvRow(line,delimiter,bad);string mpn=cell(row,mpnCol);if(bad||mpn.empty()){++stats.rejected;++stats.warnings;continue;}Statement part;sqlite3_prepare_v2(db.value,"INSERT OR IGNORE INTO parts(source_id,manufacturer,manufacturer_key,exact_mpn,mpn_key,base_part,package_variant,series,category,description,status,snapshot_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",-1,&part.value,nullptr);vector<string> pv={profile->id,profile->manufacturer,normalizeMpn(profile->manufacturer),mpn,normalizeMpn(mpn),cell(row,baseCol),cell(row,packageCol),cell(row,seriesCol),profile->category,cell(row,descriptionCol),cell(row,statusCol)};for(int i=0;i<11;++i)sqlite3_bind_text(part.value,i+1,pv[i].c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(part.value,12,stats.snapshotId);if(sqlite3_step(part.value)!=SQLITE_DONE){++stats.rejected;continue;}auto partId=sqlite3_last_insert_rowid(db.value);if(!partId)continue;++stats.parts;
    if(baseCol>=0&&!cell(row,baseCol).empty()&&normalizeMpn(cell(row,baseCol))!=normalizeMpn(mpn)){Statement a;sqlite3_prepare_v2(db.value,"INSERT OR IGNORE INTO aliases(part_id,alias,alias_key,kind) VALUES(?,?,?,'base')",-1,&a.value,nullptr);sqlite3_bind_int64(a.value,1,partId);auto v=cell(row,baseCol);auto k=normalizeMpn(v);sqlite3_bind_text(a.value,2,v.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(a.value,3,k.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(a.value)==SQLITE_DONE)++stats.aliases;}
    for(const auto& [column,mapping]:mappings){auto raw=cell(row,column);if(raw.empty())continue;auto value=parseEngineeringValue(raw,mapping.unit);if(!value.warning.empty())++stats.warnings;Statement prop;sqlite3_prepare_v2(db.value,"INSERT INTO properties(part_id,name,nominal,min_value,max_value,tolerance,unit,condition,qualifier,source_column,raw_value,raw_unit,mapping_status,profile_id,profile_version,snapshot_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",-1,&prop.value,nullptr);sqlite3_bind_int64(prop.value,1,partId);sqlite3_bind_text(prop.value,2,mapping.canonicalName.c_str(),-1,SQLITE_TRANSIENT);auto bindOptional=[&](int i,const optional<double>& v){if(v)sqlite3_bind_double(prop.value,i,*v);else sqlite3_bind_null(prop.value,i);};bindOptional(3,value.nominal);bindOptional(4,value.minimum);bindOptional(5,value.maximum);bindOptional(6,value.tolerance);vector<string> q={value.unit,value.condition,mapping.qualifier,headers[column],raw,mapping.unit,value.warning.empty()?"mapped":"warning",profile->id,profile->version};for(int i=0;i<9;++i)sqlite3_bind_text(prop.value,7+i,q[i].c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(prop.value,16,stats.snapshotId);if(sqlite3_step(prop.value)==SQLITE_DONE)++stats.properties;}
    if(options.progress&&stats.rows%500==0)options.progress(stats.rows,0,"Normalizing and indexing");
  }
  if(stats.cancelled){exec(db.value,"ROLLBACK");return stats;}Statement update;sqlite3_prepare_v2(db.value,"UPDATE snapshots SET rows_count=?,parts_count=?,aliases_count=?,properties_count=?,warnings_count=? WHERE id=?",-1,&update.value,nullptr);sqlite3_bind_int64(update.value,1,stats.rows);sqlite3_bind_int64(update.value,2,stats.parts);sqlite3_bind_int64(update.value,3,stats.aliases);sqlite3_bind_int64(update.value,4,stats.properties);sqlite3_bind_int64(update.value,5,stats.warnings);sqlite3_bind_int64(update.value,6,stats.snapshotId);sqlite3_step(update.value);if(!exec(db.value,"COMMIT")){exec(db.value,"ROLLBACK");stats.error="Catalogue transaction could not be committed";}return stats;
}
bool CatalogueDatabase::removeSource(const string& id){Db db;if(sqlite3_open(path_.string().c_str(),&db.value)!=SQLITE_OK)return false;Statement s;sqlite3_prepare_v2(db.value,"DELETE FROM snapshots WHERE source_id=?",-1,&s.value,nullptr);sqlite3_bind_text(s.value,1,id.c_str(),-1,SQLITE_TRANSIENT);return sqlite3_step(s.value)==SQLITE_DONE;}
vector<CatalogueImportStats> CatalogueDatabase::snapshots()const{vector<CatalogueImportStats> out;Db db;if(sqlite3_open_v2(path_.string().c_str(),&db.value,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)return out;Statement s;sqlite3_prepare_v2(db.value,"SELECT id,profile_id,file_hash,rows_count,parts_count,aliases_count,properties_count,warnings_count FROM snapshots ORDER BY imported_at DESC",-1,&s.value,nullptr);while(sqlite3_step(s.value)==SQLITE_ROW){CatalogueImportStats v;v.snapshotId=sqlite3_column_int64(s.value,0);v.profileId=text(s.value,1);v.fileHash=text(s.value,2);v.rows=sqlite3_column_int64(s.value,3);v.parts=sqlite3_column_int64(s.value,4);v.aliases=sqlite3_column_int64(s.value,5);v.properties=sqlite3_column_int64(s.value,6);v.warnings=sqlite3_column_int64(s.value,7);out.push_back(v);}return out;}

bool applyCatalogueEnrichment(InventoryItem& item,const CatalogueMatch& match){if(!match.matched()){string next=match.status==CatalogueMatchStatus::DatabaseUnavailable?"database_unavailable":(item.cataloguePartId.empty()?"not_in_catalogue":"stale");if(item.catalogueStatus==next)return false;item.catalogueStatus=next;return true;}string before=item.cataloguePartId+item.catalogueSnapshot+item.catalogueName+item.catalogueCategory+item.catalogueStatus;item.cataloguePartId=match.record.componentId;item.catalogueSnapshot=match.record.databaseVersion;item.catalogueName=match.record.canonicalName;item.cataloguePurposeLabel=match.record.category;item.cataloguePrintLabel=match.record.canonicalName.substr(0,16);item.catalogueCategory=match.record.category;item.catalogueDatasheetUrl=match.record.datasheetUrl;item.catalogueStatus=match.status==CatalogueMatchStatus::ExactMatch?"exact_match":"alias_match";return before!=item.cataloguePartId+item.catalogueSnapshot+item.catalogueName+item.catalogueCategory+item.catalogueStatus;}
string effectiveDatasheetUrl(const InventoryItem& item){return trim(item.datasheetUrl).empty()?item.catalogueDatasheetUrl:item.datasheetUrl;}

filesystem::path standardDownloadsFolder() {
#ifdef _WIN32
  if (const char* home = getenv("USERPROFILE")) return filesystem::path(home) / "Downloads";
#else
  if (const char* home = getenv("HOME")) return filesystem::path(home) / "Downloads";
#endif
  return {};
}

bool CatalogueDownloadSession::start(const ManufacturerSource& source, const filesystem::path& downloads) {
  cancel();
  source_ = source;
  directory_ = downloads.empty() ? standardDownloadsFolder() : downloads;
  error_code error;
  if (directory_.empty() || !filesystem::is_directory(directory_, error)) return false;
  started_ = chrono::system_clock::now();
  active_ = true;
  return true;
}

optional<filesystem::path> CatalogueDownloadSession::poll() {
  if (!active_) return nullopt;
  error_code error;
  for (filesystem::directory_iterator it(directory_, error), end; !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error)) continue;
    const auto path = it->path();
    const auto extension = lower(path.extension().string());
    if (extension == ".crdownload" || extension == ".part" || extension == ".tmp") continue;
    if (find(source_.supportedFormats.begin(), source_.supportedFormats.end(), extension) == source_.supportedFormats.end()) continue;
    const auto writeTime = filesystem::last_write_time(path, error);
    if (error) { error.clear(); continue; }
    const auto sessionFileTime = filesystem::file_time_type::clock::now() +
        chrono::duration_cast<filesystem::file_time_type::duration>(started_ - chrono::system_clock::now());
    if (writeTime < sessionFileTime) continue;
    const auto size = filesystem::file_size(path, error);
    if (error || size == 0) { error.clear(); continue; }
    auto& observation = candidates_[path];
    if (observation.first == size) ++observation.second;
    else observation = {size, 0};
    if (observation.second >= 2) { active_ = false; return path; }
  }
  return nullopt;
}

void CatalogueDownloadSession::cancel() { active_ = false; candidates_.clear(); directory_.clear(); }
bool CatalogueDownloadSession::active() const { return active_; }
const filesystem::path& CatalogueDownloadSession::watchedDirectory() const { return directory_; }
}  // namespace inventatory

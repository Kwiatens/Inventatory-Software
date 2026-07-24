// Inventatory - local manufacturer parametric catalogue storage and lookup.
#include "core/CatalogueDatabase.h"
#include "import/XlsxWorkbookReader.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
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
  {"murata-capacitors-v1","1","Murata","Capacitors","",{"murata"},{"Part Number","Product ID","MPN"},{"Series"},{"Alternate Part Number"},{"Case Code","Package"},{"Series"},{"Description"},{"Status"},{{{"Capacitance"},"capacitance","F",""},{{"Rated Voltage","Voltage"},"rated_voltage","V",""},{{"Tolerance"},"tolerance","%",""},{{"ESR"},"esr","Ohm",""},{{"Temperature Characteristic","Dielectric"},"dielectric","",""},{{"Dimensions","Size"},"dimensions","",""},{{"Operating Temperature Range","Temperature Range"},"operating_temperature","degC",""}},false},
  {"tdk-mlcc-v1","1","TDK","MLCC","",{"tdk","mlcc"},{"Part Number","Ordering Code","MPN"},{"Series"},{"Alternative"},{"Case Size","Package"},{"Series"},{"Description"},{"Status"},{{{"Capacitance"},"capacitance","F",""},{{"Rated Voltage"},"rated_voltage","V",""},{{"Tolerance"},"tolerance","%",""},{{"Temperature Characteristic"},"dielectric","",""},{{"Dissipation Factor"},"dissipation_factor","%",""},{{"Insulation Resistance"},"insulation_resistance","Ohm",""},{{"AEC-Q200"},"aec_q200","",""},{{"Dimensions","Size"},"dimensions","",""},{{"Operating Temperature Range"},"operating_temperature","degC",""},{{"Packaging"},"packaging","",""}},false},
  {"kemet-yageo-mlcc-v1","1","KEMET / Yageo","Capacitors","",{"kemet","yageo"},{"Part Number","MPN"},{"Series"},{"Alias"},{"Case","Package"},{"Series"},{"Description"},{"Status"},{{{"Capacitance"},"capacitance","F",""},{{"Voltage"},"rated_voltage","V",""},{{"Tolerance"},"tolerance","%",""},{{"Dielectric"},"dielectric","",""},{{"Dissipation Factor"},"dissipation_factor","%",""},{{"Insulation Resistance"},"insulation_resistance","Ohm",""},{{"Operating Temperature Range"},"operating_temperature","degC",""},{{"MSL"},"msl","",""},{{"AEC"},"aec_qualification","",""}},false},
  {"vishay-current-sense-v1","1","Vishay","Current-sense resistors","",{"vishay"},{"Part Number","MPN"},{},{},{"Case","Package"},{"Series"},{"Description"},{"Status"},{{{"Resistance"},"resistance","Ohm",""},{{"Tolerance"},"tolerance","%",""},{{"Power"},"power","W",""},{{"TCR"},"temperature_coefficient","ppm/degC",""}},false},
  {"nexperia-discretes-v1","1","Nexperia","Diodes, BJTs and MOSFETs","",{"nexperia"},{"Type number","Orderable part number","MPN"},{"Type number"},{"Orderable part number"},{"Package"},{"Family"},{"Description"},{"Status"},{{{"VR","VCEO","VDS"},"voltage_rating","V",""},{{"IF","IC","ID"},"current_rating","A",""},{{"IFSM"},"surge_current","A",""},{{"VF"},"forward_voltage","V",""},{{"IR","Leakage current"},"leakage_current","A",""},{{"trr","Reverse recovery time"},"reverse_recovery_time","s",""},{{"Capacitance","Cd"},"capacitance","F",""},{{"VGS"},"gate_voltage","V",""},{{"RDS(on)"},"rds_on","Ohm",""},{{"VGS(th)","Threshold voltage"},"threshold_voltage","V",""},{{"hFE","DC current gain"},"dc_current_gain","",""},{{"fT","Transition frequency"},"transition_frequency","Hz",""},{{"Ptot","Power dissipation"},"power","W",""},{{"Automotive"},"automotive_qualification","",""}},false},
  {"ti-parametric-v1","1","Texas Instruments","Amplifiers, MOSFETs and timers","Products",{"ti_","texas","opamp","mosfet","timer"},{"Orderable Part Number","Part Number","MPN"},{"Generic Part Number","Device"},{"Orderable Part Number"},{"Package Group","Package"},{"Family"},{"Description"},{"Status"},{{{"Channels"},"channel_count","",""},{{"Supply voltage (min)"},"supply_voltage_min","V","min"},{{"Supply voltage (max)"},"supply_voltage_max","V","max"},{{"Offset voltage"},"offset_voltage","V",""},{{"Offset voltage drift"},"offset_voltage_drift","V/degC",""},{{"Input bias current"},"input_bias_current","A",""},{{"GBW","Bandwidth"},"bandwidth","Hz",""},{{"Slew rate"},"slew_rate","V/us",""},{{"Iq","Quiescent current"},"quiescent_current","A",""},{{"Input noise density","Noise"},"noise_density","V/sqrtHz",""},{{"CMRR"},"cmrr","dB",""},{{"PSRR"},"psrr","dB",""},{{"Output current"},"output_current","A",""},{{"Operating temperature (min)"},"operating_temperature_min","degC","min"},{{"Operating temperature (max)"},"operating_temperature_max","degC","max"},{{"VDS"},"vds","V",""},{{"VGS"},"vgs","V",""},{{"ID"},"drain_current","A",""},{{"RDS(on)"},"rds_on","Ohm",""},{{"Threshold voltage"},"threshold_voltage","V",""},{{"Total gate charge","Qg"},"gate_charge","C",""}},false},
  {"adi-amplifiers-v1","1","Analog Devices","Precision amplifiers","",{"analogdevices","analog_devices","adi_"},{"Orderable Part Number","Model","MPN"},{"Model"},{"Orderable Part Number"},{"Package"},{"Product Family"},{"Description"},{"Status"},{{{"Channels"},"channel_count","",""},{{"Supply Voltage Min"},"supply_voltage_min","V","min"},{{"Supply Voltage Max"},"supply_voltage_max","V","max"},{{"Offset Voltage"},"offset_voltage","V",""},{{"Offset Voltage Drift"},"offset_voltage_drift","V/degC",""},{{"Input Bias Current"},"input_bias_current","A",""},{{"Input Voltage Noise Density","Voltage Noise"},"noise_density","V/sqrtHz",""},{{"Bandwidth"},"bandwidth","Hz",""},{{"Quiescent Current","Supply Current"},"quiescent_current","A",""},{{"Slew Rate"},"slew_rate","V/us",""},{{"CMRR"},"cmrr","dB",""},{{"PSRR"},"psrr","dB",""},{{"Operating Temperature Min"},"operating_temperature_min","degC","min"},{{"Operating Temperature Max"},"operating_temperature_max","degC","max"}},false},
  {"microchip-parametric-v1","1","Microchip","MCUs and amplifiers","",{"microchip"},{"Part Number","Device","MPN"},{"Device"},{"Orderable Part Number"},{"Package"},{"Family"},{"Description"},{"Status"},{{{"Program Memory","Flash Memory"},"program_memory","B",""},{{"RAM","SRAM"},"ram","B",""},{{"EEPROM"},"eeprom","B",""},{{"Pin Count"},"pin_count","",""},{{"Operating Voltage","VDD"},"operating_voltage","V",""},{{"Max Clock","CPU Frequency"},"clock","Hz",""},{{"ADC Resolution"},"adc_resolution","bit",""},{{"DAC Resolution"},"dac_resolution","bit",""},{{"Comparators"},"comparators","",""},{{"SPI"},"spi_interfaces","",""},{{"I2C"},"i2c_interfaces","",""},{{"UART"},"uart_interfaces","",""},{{"CAN"},"can_interfaces","",""},{{"USB"},"usb_interfaces","",""},{{"Timers","PWM"},"timers_pwm","",""}},false}
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
CREATE INDEX IF NOT EXISTS idx_parts_current_lookup ON parts(mpn_key,manufacturer_key,source_id,snapshot_id);
CREATE TABLE IF NOT EXISTS aliases(id INTEGER PRIMARY KEY,part_id INTEGER NOT NULL REFERENCES parts(id) ON DELETE CASCADE,alias TEXT NOT NULL,alias_key TEXT NOT NULL,kind TEXT NOT NULL,UNIQUE(part_id,alias_key));
CREATE INDEX IF NOT EXISTS idx_alias_exact ON aliases(alias_key);
CREATE TABLE IF NOT EXISTS properties(id INTEGER PRIMARY KEY,part_id INTEGER NOT NULL REFERENCES parts(id) ON DELETE CASCADE,name TEXT NOT NULL,nominal REAL,min_value REAL,typical REAL,max_value REAL,tolerance REAL,unit TEXT,condition TEXT,qualifier TEXT,source_column TEXT NOT NULL,raw_value TEXT,raw_unit TEXT,mapping_status TEXT NOT NULL,profile_id TEXT NOT NULL,profile_version TEXT NOT NULL,snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS idx_properties_part ON properties(part_id);
CREATE TABLE IF NOT EXISTS import_warnings(id INTEGER PRIMARY KEY,snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,row_number INTEGER,message TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS local_catalogue_mappings(profile_id TEXT PRIMARY KEY,profile_version TEXT NOT NULL,manufacturer TEXT NOT NULL,category TEXT NOT NULL,mpn_column TEXT NOT NULL,base_column TEXT,package_column TEXT,description_column TEXT,saved_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS local_catalogue_mapping_properties(profile_id TEXT NOT NULL REFERENCES local_catalogue_mappings(profile_id) ON DELETE CASCADE,source_column TEXT NOT NULL,canonical_name TEXT NOT NULL,unit TEXT,qualifier TEXT,PRIMARY KEY(profile_id,source_column));
)SQL"; }

bool readCsvCatalogueTable(const filesystem::path& path, CatalogueTable& table, string& error,
                           atomic_bool* cancel, const function<void(size_t, size_t, const string&)>& progress) {
  ifstream input(path, ios::binary);
  if (!input) {
    error = "Unable to open catalogue file";
    return false;
  }
  string line;
  if (!getline(input, line)) {
    error = "The CSV file is empty";
    return false;
  }
  if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
  table.delimiter = count(line.begin(), line.end(), ';') > count(line.begin(), line.end(), ',') ? ';' : ',';
  bool malformed = false;
  table.headers = parseCsvRow(line, table.delimiter, malformed);
  if (malformed || table.headers.empty()) {
    error = "The CSV header row is malformed";
    return false;
  }
  map<string, int> seen;
  for (auto& header : table.headers) header = uniqueHeader(header, seen);
  size_t row = 1;
  while (getline(input, line)) {
    if (cancel && cancel->load()) {
      error = "CSV import cancelled";
      return false;
    }
    ++row;
    bool rowMalformed = false;
    table.rows.push_back(parseCsvRow(line, table.delimiter, rowMalformed));
    if (rowMalformed) table.rows.back().push_back("\x1fMALFORMED");
    if (progress && row % 1000 == 0) progress(row, 0, "Reading CSV");
  }
  table.sourceRowCount = row;
  return true;
}

bool preserveRawColumn(const string& header) {
  const auto value = lower(trim(header));
  if (value.empty()) return false;
  const string ignored[] = {"image", "thumbnail", "photo", "export timestamp", "exported at", "generated at",
                            "table number", "row number", "navigation", "product url", "datasheet url"};
  return none_of(begin(ignored), end(ignored), [&](const string& token) { return value.find(token) != string::npos; });
}
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
const CatalogueProfile* detectCatalogueProfile(const filesystem::path& file,const vector<string>& headers){
  const string filename=lower(file.filename().string());
  const CatalogueProfile* best=nullptr; int bestScore=0; bool tie=false;
  for(const auto& profile:kProfiles){
    int score=0;
    for(const auto& pattern:profile.filenamePatterns) if(filename.find(lower(pattern))!=string::npos){score+=20;break;}
    if(findColumn(headers,profile.mpnColumns)>=0) score+=4;
    if(findColumn(headers,profile.basePartColumns)>=0) score+=2;
    if(findColumn(headers,profile.packageColumns)>=0) score+=1;
    for(const auto& mapping:profile.properties) if(findColumn(headers,mapping.columns)>=0) score+=2;
    if(score>bestScore){best=&profile;bestScore=score;tie=false;} else if(score==bestScore&&score>0){tie=true;}
  }
  // A generic MPN column alone is not sufficient evidence. File names may be
  // trusted, otherwise require at least an identity column plus one profile
  // specific signal and reject ties for the manual mapping path.
  if(!best || tie || (bestScore<20 && bestScore<6)) return nullptr;
  return best;
}

EngineeringValue parseEngineeringValue(const string& input,const string& contextUnit) {
  EngineeringValue out; out.rawValue=input; out.originalText=input; out.rawUnit=contextUnit; string s=trim(input); if(s.empty()) return out;
  for(const string micro : {"µ", "μ"}) for(size_t p;(p=s.find(micro))!=string::npos;) s.replace(p,micro.size(),"u");
  for(const string dash : {"–", "—"}) for(size_t p;(p=s.find(dash))!=string::npos;) s.replace(p,dash.size(),"-");
  smatch m; static const regex range(R"(^\s*([+-]?\d+(?:\.\d+)?)\s*(?:to|-)\s*([+-]?\d+(?:\.\d+)?)\s*(.*)$)",regex::icase);
  regex tolerance(R"(^\s*[+\-±]\s*(\d+(?:\.\d+)?)\s*%\s*$)");
  if(regex_match(s,m,tolerance)){out.tolerance=stod(m[1]);out.unit="%";return out;}
  string number,unit;
  if(regex_match(s,m,range)){out.minimum=stod(m[1]);out.maximum=stod(m[2]);unit=trim(m[3]);}
  else { static const regex scalar(R"(^\s*([+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*([^\s]*)\s*$)"); if(!regex_match(s,m,scalar)){out.warning="Ambiguous engineering value";return out;} out.nominal=stod(m[1]); unit=trim(m[2]); }
  if(unit.empty()) unit=contextUnit; if(unit=="VDC")unit="V"; if(unit=="Ω")unit="Ohm"; if(unit=="°C")unit="degC";
  static const map<string,double> prefixes={{"p",1e-12},{"n",1e-9},{"u",1e-6},{"m",1e-3},{"k",1e3},{"K",1e3},{"M",1e6},{"G",1e9}};
  if(unit.size()==1 && prefixes.count(unit) && contextUnit.empty()){out.warning="Unit prefix has no base unit";return out;}
  string base=unit; double factor=1; for(const auto& [prefix,f]:prefixes) if(unit.size()>prefix.size() && unit.rfind(prefix,0)==0){base=unit.substr(prefix.size());factor=f;break;}
  if(unit.size()==1 && prefixes.count(unit) && !contextUnit.empty()){factor=prefixes.at(unit);base=contextUnit;}
  if(base.empty()){out.warning="Unit is ambiguous without column context";return out;} out.unit=base;
  if(out.nominal)*out.nominal*=factor;if(out.minimum)*out.minimum*=factor;if(out.maximum)*out.maximum*=factor; return out;
}

bool CatalogueMatch::matched() const{return status==CatalogueMatchStatus::ExactMatch||status==CatalogueMatchStatus::AliasMatch;}
CatalogueDatabase::~CatalogueDatabase() { closeReadConnection(); }
void CatalogueDatabase::closeReadConnection() const { lock_guard<mutex> lock(readMutex_); if (readDb_) { sqlite3_close(readDb_); readDb_ = nullptr; } }
bool CatalogueDatabase::open(const filesystem::path& path){closeReadConnection();path_=path;filesystem::create_directories(path.parent_path());Db db;if(sqlite3_open(path.string().c_str(),&db.value)!=SQLITE_OK)return false;sqlite3_busy_timeout(db.value,5000);available_=exec(db.value,schema());version_="1";return available_;}
bool CatalogueDatabase::available()const{return available_;} const string& CatalogueDatabase::version()const{return version_;}

CatalogueMatch CatalogueDatabase::lookup(const string& manufacturer,const string& mpn)const {
  if(!available_)return {CatalogueMatchStatus::DatabaseUnavailable,{}}; lock_guard<mutex> lock(readMutex_);
  if (!readDb_) { sqlite3* connection = nullptr; if (sqlite3_open_v2(path_.string().c_str(), &connection, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) == SQLITE_OK) { readDb_ = connection; sqlite3_busy_timeout(readDb_, 5000); } else if (connection) sqlite3_close(connection); }
  sqlite3* db = readDb_; if(!db)return {CatalogueMatchStatus::DatabaseUnavailable,{}};
  const string mk=normalizeMpn(manufacturer), pk=normalizeMpn(mpn); if(pk.empty())return {};
  const auto loadProperties = [&](CatalogueRecord& record) {
    Statement properties;
    sqlite3_prepare_v2(db, "SELECT name,nominal,min_value,typical,max_value,tolerance,unit,condition,qualifier,source_column,raw_value,raw_unit,mapping_status FROM properties WHERE part_id=? ORDER BY mapping_status,name", -1, &properties.value, nullptr);
    sqlite3_bind_int64(properties.value, 1, stoll(record.componentId));
    while (sqlite3_step(properties.value) == SQLITE_ROW) {
      CatalogueProperty property;
      property.name = text(properties.value, 0);
      const auto optionalNumber = [&](int column) -> optional<double> {
        return sqlite3_column_type(properties.value, column) == SQLITE_NULL ? nullopt : optional<double>(sqlite3_column_double(properties.value, column));
      };
      property.value.nominal = optionalNumber(1); property.value.minimum = optionalNumber(2); property.value.typical = optionalNumber(3);
      property.value.maximum = optionalNumber(4); property.value.tolerance = optionalNumber(5); property.value.unit = text(properties.value, 6);
      property.condition = property.value.condition = text(properties.value, 7); property.qualifier = property.value.qualifier = text(properties.value, 8);
      property.sourceColumn = text(properties.value, 9); property.rawValue = property.value.rawValue = text(properties.value, 10);
      property.originalUnit = property.value.rawUnit = text(properties.value, 11); property.mappingStatus = text(properties.value, 12);
      record.properties.push_back(move(property));
    }
  };
  auto run=[&](bool alias)->CatalogueMatch { Statement s; string sql=alias?"SELECT p.id,p.manufacturer,p.exact_mpn,p.base_part,p.description,p.category,p.package_variant,p.packaging_variant,p.series,p.snapshot_id FROM aliases a JOIN parts p ON p.id=a.part_id WHERE a.alias_key=? AND (?='' OR p.manufacturer_key=?) AND p.snapshot_id=(SELECT max(s2.id) FROM snapshots s2 WHERE s2.source_id=p.source_id) ORDER BY p.snapshot_id DESC LIMIT 2":"SELECT id,manufacturer,exact_mpn,base_part,description,category,package_variant,packaging_variant,series,snapshot_id FROM parts WHERE mpn_key=? AND (?='' OR manufacturer_key=?) AND snapshot_id=(SELECT max(s2.id) FROM snapshots s2 WHERE s2.source_id=parts.source_id) ORDER BY snapshot_id DESC LIMIT 2"; sqlite3_prepare_v2(db,sql.c_str(),-1,&s.value,nullptr);sqlite3_bind_text(s.value,1,pk.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s.value,2,mk.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s.value,3,mk.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(s.value)!=SQLITE_ROW)return {};CatalogueRecord r;r.componentId=to_string(sqlite3_column_int64(s.value,0));r.manufacturer=text(s.value,1);r.manufacturerPartNumber=text(s.value,2);r.basePart=text(s.value,3);r.canonicalName=text(s.value,4);r.category=text(s.value,5);r.packageVariant=text(s.value,6);r.packagingVariant=text(s.value,7);r.series=text(s.value,8);r.databaseVersion=to_string(sqlite3_column_int64(s.value,9));if(sqlite3_step(s.value)==SQLITE_ROW)return {CatalogueMatchStatus::Ambiguous,{}};return {alias?CatalogueMatchStatus::AliasMatch:CatalogueMatchStatus::ExactMatch,r};};
  auto result = run(false);
  if (result.status == CatalogueMatchStatus::NotFound) result = run(true);
  if (result.matched()) loadProperties(result.record);
  return result;
}

CatalogueImportStats importCatalogueTable(const filesystem::path& databasePath, const filesystem::path& path,
                                          const CatalogueProfile* selected, const CatalogueImportOptions& options) {
  CatalogueImportStats stats;
  CatalogueTable table;
  const auto extension = lower(path.extension().string());
  string readError;
  const bool loaded = extension == ".csv"
                          ? readCsvCatalogueTable(path, table, readError, options.cancel, options.progress)
                          : extension == ".xlsx"
                                ? readXlsxCatalogueTable(path, selected ? selected->worksheetPattern : "", table, readError,
                                                         options.cancel, options.progress)
                                : false;
  if (!loaded) {
    stats.error = readError.empty() ? "Only CSV and XLSX catalogue files are supported" : readError;
    stats.cancelled = options.cancel && options.cancel->load();
    return stats;
  }
  map<string, int> seen;
  for (auto& header : table.headers) header = uniqueHeader(header, seen);
  const auto* profile = selected ? selected : detectCatalogueProfile(path, table.headers);
  if (!profile) {
    stats.error = "No manufacturer profile matches this file";
    return stats;
  }
  stats.profileId = profile->id;
  const int mpnCol = findColumn(table.headers, profile->mpnColumns);
  if (mpnCol < 0 && !profile->seriesOnly) {
    stats.error = "The expected MPN column is missing";
    return stats;
  }
  const int baseCol = findColumn(table.headers, profile->basePartColumns);
  const int packageCol = findColumn(table.headers, profile->packageColumns);
  const int seriesCol = findColumn(table.headers, profile->seriesColumns);
  const int descriptionCol = findColumn(table.headers, profile->descriptionColumns);
  const int statusCol = findColumn(table.headers, profile->statusColumns);
  vector<pair<int, PropertyMapping>> mappings;
  set<int> mappedColumns;
  for (const auto& mapping : profile->properties) {
    const int column = findColumn(table.headers, mapping.columns);
    if (column >= 0) {
      mappings.emplace_back(column, mapping);
      mappedColumns.insert(column);
    }
  }
  const set<int> structuralColumns = {mpnCol, baseCol, packageCol, seriesCol, descriptionCol, statusCol};
  Db db;if(sqlite3_open(databasePath.string().c_str(),&db.value)!=SQLITE_OK){stats.error="Unable to open catalogue database";return stats;}sqlite3_busy_timeout(db.value,5000);if(!exec(db.value,"BEGIN IMMEDIATE")){stats.error="Catalogue database is busy";return stats;}const string hash=hashFile(path);stats.fileHash=hash;Statement duplicate;sqlite3_prepare_v2(db.value,"SELECT id FROM snapshots WHERE file_hash=?",-1,&duplicate.value,nullptr);sqlite3_bind_text(duplicate.value,1,hash.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(duplicate.value)==SQLITE_ROW){exec(db.value,"ROLLBACK");stats.duplicate=true;stats.snapshotId=sqlite3_column_int64(duplicate.value,0);return stats;}
  Statement snap;sqlite3_prepare_v2(db.value,"INSERT INTO snapshots(source_id,manufacturer,profile_id,profile_version,filename,file_hash,imported_at) VALUES(?,?,?,?,?,?,?)",-1,&snap.value,nullptr);vector<string> sv={profile->id,profile->manufacturer,profile->id,profile->version,path.filename().string(),hash};for(int i=0;i<6;++i)sqlite3_bind_text(snap.value,i+1,sv[i].c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(snap.value,7,time(nullptr));if(sqlite3_step(snap.value)!=SQLITE_DONE){exec(db.value,"ROLLBACK");stats.error="Unable to create catalogue snapshot";return stats;}stats.snapshotId=sqlite3_last_insert_rowid(db.value);
  Statement part, alias, property, warning;
  sqlite3_prepare_v2(db.value, "INSERT OR IGNORE INTO parts(source_id,manufacturer,manufacturer_key,exact_mpn,mpn_key,base_part,package_variant,series,category,description,status,snapshot_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", -1, &part.value, nullptr);
  sqlite3_prepare_v2(db.value, "INSERT OR IGNORE INTO aliases(part_id,alias,alias_key,kind) VALUES(?,?,?,?)", -1, &alias.value, nullptr);
  sqlite3_prepare_v2(db.value, "INSERT INTO properties(part_id,name,nominal,min_value,typical,max_value,tolerance,unit,condition,qualifier,source_column,raw_value,raw_unit,mapping_status,profile_id,profile_version,snapshot_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &property.value, nullptr);
  sqlite3_prepare_v2(db.value, "INSERT INTO import_warnings(snapshot_id,row_number,message) VALUES(?,?,?)", -1, &warning.value, nullptr);
  const auto bindOptional = [&](int index, const optional<double>& value) {
    if (value) sqlite3_bind_double(property.value, index, *value); else sqlite3_bind_null(property.value, index);
  };
  const auto addWarning = [&](size_t row, const string& message) {
    sqlite3_reset(warning.value); sqlite3_clear_bindings(warning.value);
    sqlite3_bind_int64(warning.value, 1, stats.snapshotId); sqlite3_bind_int64(warning.value, 2, static_cast<sqlite3_int64>(row));
    sqlite3_bind_text(warning.value, 3, message.c_str(), -1, SQLITE_TRANSIENT); sqlite3_step(warning.value);
    ++stats.warnings;
  };
  const auto addProperty = [&](sqlite3_int64 partId, const string& name, const string& sourceColumn, const string& raw,
                               const EngineeringValue* value, const string& mappingStatus, const string& qualifier) {
    sqlite3_reset(property.value); sqlite3_clear_bindings(property.value);
    sqlite3_bind_int64(property.value, 1, partId); sqlite3_bind_text(property.value, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    if (value) { bindOptional(3, value->nominal); bindOptional(4, value->minimum); bindOptional(5, value->typical); bindOptional(6, value->maximum); bindOptional(7, value->tolerance); }
    else for (int index = 3; index <= 7; ++index) sqlite3_bind_null(property.value, index);
    const string unit = value ? value->unit : ""; const string condition = value ? value->condition : "";
    sqlite3_bind_text(property.value, 8, unit.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(property.value, 9, condition.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 10, qualifier.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(property.value, 11, sourceColumn.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 12, raw.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(property.value, 13, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 14, mappingStatus.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(property.value, 15, profile->id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 16, profile->version.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(property.value, 17, stats.snapshotId);
    if (sqlite3_step(property.value) == SQLITE_DONE) { ++stats.properties; if (mappingStatus == "unmapped") ++stats.unmappedProperties; }
  };
  for (size_t index = 0; index < table.rows.size(); ++index) {
    if (options.cancel && options.cancel->load()) { stats.cancelled = true; break; }
    const auto& row = table.rows[index];
    if (row.empty() || all_of(row.begin(), row.end(), [](const string& value) { return trim(value).empty(); })) continue;
    ++stats.rows;
    const auto sourceRow = table.headerRow + index + 2;
    if (!row.empty() && row.back() == "\x1fMALFORMED") { ++stats.rejected; addWarning(sourceRow, "Malformed CSV row"); continue; }
    const string mpn = cell(row, mpnCol);
    if (mpn.empty() && !profile->seriesOnly) { ++stats.rejected; addWarning(sourceRow, "Missing manufacturer part number"); continue; }
    sqlite3_reset(part.value); sqlite3_clear_bindings(part.value);
    const vector<string> partValues = {profile->id, profile->manufacturer, normalizeMpn(profile->manufacturer), mpn, normalizeMpn(mpn), cell(row, baseCol), cell(row, packageCol), cell(row, seriesCol), profile->category, cell(row, descriptionCol), cell(row, statusCol)};
    for (int bind = 0; bind < 11; ++bind) sqlite3_bind_text(part.value, bind + 1, partValues[bind].c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(part.value, 12, stats.snapshotId);
    if (sqlite3_step(part.value) != SQLITE_DONE || sqlite3_changes(db.value) == 0) { ++stats.rejected; addWarning(sourceRow, "Duplicate or invalid catalogue part"); continue; }
    const auto partId = sqlite3_last_insert_rowid(db.value); ++stats.parts;
    const auto addAlias = [&](const string& aliasValue, const char* kind) {
      if (aliasValue.empty() || normalizeMpn(aliasValue) == normalizeMpn(mpn)) return;
      sqlite3_reset(alias.value); sqlite3_clear_bindings(alias.value); const auto key = normalizeMpn(aliasValue);
      sqlite3_bind_int64(alias.value, 1, partId); sqlite3_bind_text(alias.value, 2, aliasValue.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(alias.value, 3, key.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(alias.value, 4, kind, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(alias.value) == SQLITE_DONE && sqlite3_changes(db.value) != 0) ++stats.aliases;
    };
    if (!profile->seriesOnly) addAlias(cell(row, baseCol), "base");
    for (const auto& [column, mapping] : mappings) {
      const auto raw = cell(row, column); if (raw.empty()) continue;
      const auto value = parseEngineeringValue(raw, mapping.unit);
      addProperty(partId, mapping.canonicalName, table.headers[column], raw, &value, value.warning.empty() ? "mapped" : "warning", mapping.qualifier);
      if (!value.warning.empty()) addWarning(sourceRow, table.headers[column] + ": " + value.warning);
    }
    for (size_t column = 0; column < table.headers.size(); ++column) {
      if (mappedColumns.count(static_cast<int>(column)) || structuralColumns.count(static_cast<int>(column)) || !preserveRawColumn(table.headers[column])) continue;
      const auto raw = cell(row, static_cast<int>(column)); if (!raw.empty()) addProperty(partId, table.headers[column], table.headers[column], raw, nullptr, "unmapped", "");
    }
    if (options.progress && stats.rows % 500 == 0) options.progress(stats.rows, table.rows.size(), "Normalizing and indexing");
  }
  if(stats.cancelled){exec(db.value,"ROLLBACK");return stats;}Statement update;sqlite3_prepare_v2(db.value,"UPDATE snapshots SET rows_count=?,parts_count=?,aliases_count=?,properties_count=?,warnings_count=? WHERE id=?",-1,&update.value,nullptr);sqlite3_bind_int64(update.value,1,stats.rows);sqlite3_bind_int64(update.value,2,stats.parts);sqlite3_bind_int64(update.value,3,stats.aliases);sqlite3_bind_int64(update.value,4,stats.properties);sqlite3_bind_int64(update.value,5,stats.warnings);sqlite3_bind_int64(update.value,6,stats.snapshotId);sqlite3_step(update.value);if(!exec(db.value,"COMMIT")){exec(db.value,"ROLLBACK");stats.error="Catalogue transaction could not be committed";}return stats;
}

CatalogueImportStats CatalogueDatabase::importFile(const filesystem::path& path, const CatalogueProfile* profile,
                                                    const CatalogueImportOptions& options) {
  if (!available_) {
    CatalogueImportStats stats;
    stats.error = "Catalogue database is unavailable";
    return stats;
  }
  closeReadConnection();
  return importCatalogueTable(path_, path, profile, options);
}

bool CatalogueDatabase::saveLocalMapping(const CatalogueProfile& profile) {
  if (!available_ || profile.id.empty() || profile.mpnColumns.empty()) return false;
  Db db;
  if (sqlite3_open(path_.string().c_str(), &db.value) != SQLITE_OK) return false;
  Statement statement;
  sqlite3_prepare_v2(db.value, "INSERT INTO local_catalogue_mappings(profile_id,profile_version,manufacturer,category,mpn_column,base_column,package_column,description_column,saved_at) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(profile_id) DO UPDATE SET profile_version=excluded.profile_version,manufacturer=excluded.manufacturer,category=excluded.category,mpn_column=excluded.mpn_column,base_column=excluded.base_column,package_column=excluded.package_column,description_column=excluded.description_column,saved_at=excluded.saved_at", -1, &statement.value, nullptr);
  const auto column = [](const vector<string>& values) { return values.empty() ? string{} : values.front(); };
  const array<string, 8> values = {profile.id, profile.version, profile.manufacturer, profile.category, column(profile.mpnColumns), column(profile.basePartColumns), column(profile.packageColumns), column(profile.descriptionColumns)};
  for (int index = 0; index < 8; ++index) sqlite3_bind_text(statement.value, index + 1, values[index].c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.value, 9, time(nullptr));
  if (sqlite3_step(statement.value) != SQLITE_DONE) return false;
  Statement remove;
  sqlite3_prepare_v2(db.value, "DELETE FROM local_catalogue_mapping_properties WHERE profile_id=?", -1, &remove.value, nullptr);
  sqlite3_bind_text(remove.value, 1, profile.id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(remove.value) != SQLITE_DONE) return false;
  Statement property;
  sqlite3_prepare_v2(db.value, "INSERT INTO local_catalogue_mapping_properties(profile_id,source_column,canonical_name,unit,qualifier) VALUES(?,?,?,?,?)", -1, &property.value, nullptr);
  for (const auto& mapping : profile.properties) {
    if (mapping.columns.empty()) continue;
    sqlite3_reset(property.value); sqlite3_clear_bindings(property.value);
    sqlite3_bind_text(property.value, 1, profile.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 2, mapping.columns.front().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 3, mapping.canonicalName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 4, mapping.unit.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(property.value, 5, mapping.qualifier.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(property.value) != SQLITE_DONE) return false;
  }
  return true;
}

optional<CatalogueProfile> CatalogueDatabase::localMapping(const string& profileId) const {
  if (!available_) return nullopt;
  Db db;
  if (sqlite3_open_v2(path_.string().c_str(), &db.value, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return nullopt;
  Statement statement;
  sqlite3_prepare_v2(db.value, "SELECT profile_version,manufacturer,category,mpn_column,base_column,package_column,description_column FROM local_catalogue_mappings WHERE profile_id=?", -1, &statement.value, nullptr);
  sqlite3_bind_text(statement.value, 1, profileId.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(statement.value) != SQLITE_ROW) return nullopt;
  const auto optionalColumn = [&](int index) { const auto value = text(statement.value, index); return value.empty() ? vector<string>{} : vector<string>{value}; };
  CatalogueProfile profile{profileId, text(statement.value, 0), text(statement.value, 1), text(statement.value, 2), "", {},
                           optionalColumn(3), optionalColumn(4), {}, optionalColumn(5), {}, optionalColumn(6), {}, {}, false};
  Statement properties;
  sqlite3_prepare_v2(db.value, "SELECT source_column,canonical_name,unit,qualifier FROM local_catalogue_mapping_properties WHERE profile_id=? ORDER BY source_column", -1, &properties.value, nullptr);
  sqlite3_bind_text(properties.value, 1, profileId.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(properties.value) == SQLITE_ROW) profile.properties.push_back({{text(properties.value, 0)}, text(properties.value, 1), text(properties.value, 2), text(properties.value, 3)});
  return profile;
}

CatalogueImportPreview CatalogueDatabase::previewFile(const filesystem::path& path, const CatalogueProfile* selected,
                                                       atomic_bool* cancel) const {
  CatalogueImportPreview preview;
  preview.filename = path.filename().string();
  preview.format = lower(path.extension().string());
  error_code sizeError;
  preview.fileSize = filesystem::file_size(path, sizeError);
  CatalogueTable table;
  const auto loaded = preview.format == ".csv"
                          ? readCsvCatalogueTable(path, table, preview.error, cancel, {})
                          : preview.format == ".xlsx"
                                ? readXlsxCatalogueTable(path, selected ? selected->worksheetPattern : "", table, preview.error,
                                                         cancel, {})
                                : false;
  if (!loaded) {
    if (preview.error.empty()) preview.error = "Only CSV and XLSX catalogue files are supported";
    return preview;
  }
  map<string, int> seen;
  for (auto& header : table.headers) header = uniqueHeader(header, seen);
  const auto* profile = selected ? selected : detectCatalogueProfile(path, table.headers);
  preview.sheetName = table.sheetName;
  preview.headerRow = table.headerRow;
  preview.rows = table.rows.size();
  preview.delimiter = table.delimiter;
  preview.headers = table.headers;
  preview.sampleRows.assign(table.rows.begin(), table.rows.begin() + min<size_t>(10, table.rows.size()));
  if (!profile) {
    preview.warnings.push_back("No known manufacturer profile matched this table; map its identity columns before importing.");
    return preview;
  }
  preview.profileId = profile->id;
  preview.profileVersion = profile->version;
  const set<int> structural = {findColumn(table.headers, profile->mpnColumns), findColumn(table.headers, profile->basePartColumns),
                               findColumn(table.headers, profile->packageColumns), findColumn(table.headers, profile->seriesColumns),
                               findColumn(table.headers, profile->descriptionColumns), findColumn(table.headers, profile->statusColumns)};
  set<int> mapped;
  for (const auto& mapping : profile->properties) {
    const int column = findColumn(table.headers, mapping.columns);
    if (column >= 0) { mapped.insert(column); preview.mappedColumns.push_back(table.headers[column]); }
  }
  if (findColumn(table.headers, profile->mpnColumns) < 0 && !profile->seriesOnly) preview.warnings.push_back("Expected MPN column is missing");
  for (size_t column = 0; column < table.headers.size(); ++column) {
    if (!mapped.count(static_cast<int>(column)) && !structural.count(static_cast<int>(column)) && preserveRawColumn(table.headers[column])) {
      preview.unmappedColumns.push_back(table.headers[column]);
    }
  }
  return preview;
}

size_t CatalogueDatabase::reprocessSource(const CatalogueProfile& profile) {
  if (!available_) return 0;
  closeReadConnection();
  Db db;
  if (sqlite3_open(path_.string().c_str(), &db.value) != SQLITE_OK || !exec(db.value, "BEGIN IMMEDIATE")) return 0;
  Statement select;
  sqlite3_prepare_v2(db.value, R"SQL(
    SELECT properties.id, properties.source_column, properties.raw_value
    FROM properties JOIN snapshots ON snapshots.id=properties.snapshot_id
    WHERE snapshots.source_id=? AND properties.mapping_status='unmapped'
  )SQL", -1, &select.value, nullptr);
  sqlite3_bind_text(select.value, 1, profile.id.c_str(), -1, SQLITE_TRANSIENT);
  Statement update;
  sqlite3_prepare_v2(db.value, R"SQL(
    UPDATE properties SET name=?,nominal=?,min_value=?,typical=?,max_value=?,tolerance=?,unit=?,condition=?,qualifier=?,
      raw_unit=?,mapping_status='mapped',profile_id=?,profile_version=? WHERE id=?
  )SQL", -1, &update.value, nullptr);
  const auto bindOptional = [&](int index, const optional<double>& value) {
    if (value) sqlite3_bind_double(update.value, index, *value); else sqlite3_bind_null(update.value, index);
  };
  size_t remapped = 0;
  while (sqlite3_step(select.value) == SQLITE_ROW) {
    const auto sourceColumn = text(select.value, 1);
    const auto found = find_if(profile.properties.begin(), profile.properties.end(), [&](const PropertyMapping& mapping) {
      return any_of(mapping.columns.begin(), mapping.columns.end(), [&](const string& column) {
        return lower(trim(column)) == lower(trim(sourceColumn));
      });
    });
    if (found == profile.properties.end()) continue;
    const auto value = parseEngineeringValue(text(select.value, 2), found->unit);
    if (!value.warning.empty()) continue;
    sqlite3_reset(update.value); sqlite3_clear_bindings(update.value);
    sqlite3_bind_text(update.value, 1, found->canonicalName.c_str(), -1, SQLITE_TRANSIENT);
    bindOptional(2, value.nominal); bindOptional(3, value.minimum); bindOptional(4, value.typical);
    bindOptional(5, value.maximum); bindOptional(6, value.tolerance);
    sqlite3_bind_text(update.value, 7, value.unit.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 8, value.condition.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 9, found->qualifier.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 10, found->unit.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 11, profile.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 12, profile.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.value, 13, sqlite3_column_int64(select.value, 0));
    if (sqlite3_step(update.value) == SQLITE_DONE && sqlite3_changes(db.value) != 0) ++remapped;
  }
  if (!exec(db.value, "COMMIT")) { exec(db.value, "ROLLBACK"); return 0; }
  return remapped;
}

bool CatalogueDatabase::removeSource(const string& id) {
  closeReadConnection();
  Db db;
  if (sqlite3_open(path_.string().c_str(), &db.value) != SQLITE_OK) return false;
  string profileId = id;
  for (const auto& source : manufacturerSources()) {
    if (source.id == id) {
      profileId = source.profileId;
      break;
    }
  }
  Statement statement;
  sqlite3_prepare_v2(db.value, "DELETE FROM snapshots WHERE source_id=?", -1, &statement.value, nullptr);
  sqlite3_bind_text(statement.value, 1, profileId.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(statement.value) == SQLITE_DONE;
}
vector<CatalogueImportStats> CatalogueDatabase::snapshots()const{vector<CatalogueImportStats> out;Db db;if(sqlite3_open_v2(path_.string().c_str(),&db.value,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)return out;Statement s;sqlite3_prepare_v2(db.value,"SELECT id,profile_id,profile_version,filename,file_hash,imported_at,rows_count,parts_count,aliases_count,properties_count,warnings_count FROM snapshots ORDER BY imported_at DESC",-1,&s.value,nullptr);while(sqlite3_step(s.value)==SQLITE_ROW){CatalogueImportStats v;v.snapshotId=sqlite3_column_int64(s.value,0);v.profileId=text(s.value,1);v.profileVersion=text(s.value,2);v.filename=text(s.value,3);v.fileHash=text(s.value,4);v.importedAt=sqlite3_column_int64(s.value,5);v.rows=sqlite3_column_int64(s.value,6);v.parts=sqlite3_column_int64(s.value,7);v.aliases=sqlite3_column_int64(s.value,8);v.properties=sqlite3_column_int64(s.value,9);v.warnings=sqlite3_column_int64(s.value,10);out.push_back(v);}return out;}
vector<CatalogueWarning> CatalogueDatabase::warningsForSnapshot(int64_t snapshotId)const{vector<CatalogueWarning> out;Db db;if(sqlite3_open_v2(path_.string().c_str(),&db.value,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK)return out;Statement s;sqlite3_prepare_v2(db.value,"SELECT snapshot_id,row_number,message FROM import_warnings WHERE snapshot_id=? ORDER BY row_number,id",-1,&s.value,nullptr);sqlite3_bind_int64(s.value,1,snapshotId);while(sqlite3_step(s.value)==SQLITE_ROW)out.push_back({sqlite3_column_int64(s.value,0),static_cast<size_t>(sqlite3_column_int64(s.value,1)),text(s.value,2)});return out;}

bool applyCatalogueEnrichment(InventoryItem& item,const CatalogueMatch& match){if(!match.matched()){const string before=item.cataloguePartId+item.catalogueSnapshot+item.catalogueName+item.cataloguePurposeLabel+item.cataloguePrintLabel+item.catalogueCategory+item.catalogueDatasheetUrl+item.catalogueStatus;item.cataloguePartId.clear();item.catalogueSnapshot.clear();item.catalogueName.clear();item.cataloguePurposeLabel.clear();item.cataloguePrintLabel.clear();item.catalogueCategory.clear();item.catalogueDatasheetUrl.clear();item.catalogueStatus=match.status==CatalogueMatchStatus::DatabaseUnavailable?"database_unavailable":"not_in_catalogue";return before!=item.cataloguePartId+item.catalogueSnapshot+item.catalogueName+item.cataloguePurposeLabel+item.cataloguePrintLabel+item.catalogueCategory+item.catalogueDatasheetUrl+item.catalogueStatus;}string before=item.cataloguePartId+item.catalogueSnapshot+item.catalogueName+item.catalogueCategory+item.catalogueStatus;item.cataloguePartId=match.record.componentId;item.catalogueSnapshot=match.record.databaseVersion;item.catalogueName=match.record.canonicalName;item.cataloguePurposeLabel=match.record.category;item.cataloguePrintLabel=match.record.canonicalName.substr(0,16);item.catalogueCategory=match.record.category;item.catalogueDatasheetUrl=match.record.datasheetUrl;item.catalogueStatus=match.status==CatalogueMatchStatus::ExactMatch?"exact_match":"alias_match";return before!=item.cataloguePartId+item.catalogueSnapshot+item.catalogueName+item.catalogueCategory+item.catalogueStatus;}
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

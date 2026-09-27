#!/usr/bin/env python3
"""Exercise the production selected-update control branch with versioned providers."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import os

parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2])
parser.add_argument('--cc',default='cl')
args=parser.parse_args()
source=(args.source/'src/suyu/game_export.cpp').read_text()
start=source.index('if (installed && installed->slot)',source.index('static FileSys::VirtualFile ExportControlNca'))
end=source.index('{',start)+1
depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}')
    end+=1
branch=source[start:end]
harness=r'''
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <iostream>
using u64=unsigned long long;
namespace FileSys {
using VirtualFile=std::shared_ptr<std::string>;
enum class ContentRecordType { Control };
enum class ContentProviderUnionSlot { External, FrontendManual, UserNAND };
struct Provider {
 VirtualFile raw=std::make_shared<std::string>("newest");
 mutable int raw_calls=0,version_calls=0;
 VirtualFile GetEntryRaw(u64,ContentRecordType)const{++raw_calls;return raw;}
};
struct ManualContentProvider:Provider {
 bool versioned=true,missing_selected=false;
 std::vector<int> ListUpdateVersions(u64)const{return versioned?std::vector<int>{2,1}:std::vector<int>{};}
 VirtualFile GetEntryForVersion(u64,ContentRecordType,unsigned version)const{
  ++version_calls;assert(version==1);return missing_selected?nullptr:std::make_shared<std::string>("selected-older");
 }
};
struct ExternalContentProvider:ManualContentProvider {};
}
struct Union {
 FileSys::Provider* provider;
 const FileSys::Provider* GetSlotProvider(FileSys::ContentProviderUnionSlot)const{return provider;}
};
struct System { Union content; const Union& GetContentProviderUnion()const{return content;} };
struct Update {std::optional<FileSys::ContentProviderUnionSlot> slot;unsigned version=1;};
FileSys::VirtualFile select(System& system,const std::optional<Update>& installed){
 const u64 update_id=0x800;
'''+branch+r'''
 return std::make_shared<std::string>("base");
}
int main(){
 using S=FileSys::ContentProviderUnionSlot;
 FileSys::ManualContentProvider manual;FileSys::ExternalContentProvider external;FileSys::Provider nand;
 for(auto slot:{S::External,S::FrontendManual}){
  auto& provider=slot==S::External?static_cast<FileSys::ManualContentProvider&>(external):manual;
  System system{{&provider}};std::optional<Update> installed=Update{slot,1};
  assert(*select(system,installed)=="selected-older");assert(provider.raw_calls==0&&provider.version_calls==1);
  provider.missing_selected=true;assert(*select(system,installed)=="base");assert(provider.raw_calls==0);
  provider.versioned=false;assert(*select(system,installed)=="newest");assert(provider.raw_calls==1);
 }
 System system{{&nand}};assert(*select(system,Update{S::UserNAND,1})=="newest");
 assert(*select(system,std::nullopt)=="base");
 std::cout<<"PASS selected older Control version, selected version missing fallback, unversioned manual/external, NAND and no update\n";
}
'''
with tempfile.TemporaryDirectory(prefix='suyu-control-test-') as tmp:
    directory=Path(tmp)
    cpp=directory/'test.cpp';cpp.write_text(harness)
    exe=directory/('test.exe' if os.name=='nt' else 'test')
    command=([args.cc,'/nologo','/EHsc','/std:c++20',str(cpp),f'/Fe:{exe}',f'/Fo:{directory / "test.obj"}'] if os.name=='nt'
             else [args.cc,'-std=c++20',str(cpp),'-o',str(exe)])
    subprocess.run(command,check=True)
    subprocess.run([str(exe)],check=True)

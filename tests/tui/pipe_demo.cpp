// pipe_demo.cpp — streaming protocol pipeline: raw XML → render
// Usage: echo '<action ...>...</action>' | ./pipe_demo
//        ./pipe_demo tests/tui/presets/07_tool_action_result.tui  (reads <stream> section)
#include "../../src/tui/components/protocol.hpp"
#include "../../src/tui/components/markdown.hpp"
#include "../../src/tui/terminal.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <map>
#include <fstream>
using namespace cortex::mk3::tui;

struct Tag { std::string name; std::map<std::string,std::string> a; bool close=false; };
Tag parse(const std::string& raw) {
    Tag t; if(raw.size()<3||raw[0]!='<')return t;
    size_t s=1; if(raw[s]=='/'){t.close=true;s++;}
    size_t sp=raw.find_first_of(" >/",s); t.name=raw.substr(s,sp-s);
    if(sp!=std::string::npos&&raw[sp]==' '){
        for(size_t p=sp+1;p<raw.size();){
            size_t e=raw.find('=',p); if(e==std::string::npos)break;
            std::string k=raw.substr(p,e-p);
            while(!k.empty()&&k.front()==' ')k.erase(0,1);
            while(!k.empty()&&k.back()==' ')k.pop_back();
            size_t vs=raw.find('"',e); if(vs==std::string::npos)break; vs++;
            size_t ve=raw.find('"',vs); if(ve==std::string::npos)break;
            t.a[k]=raw.substr(vs,ve-vs); p=ve+1;
        }
    } return t;
}

int main(int argc, char** argv) {
    std::string input;
    if(argc>1){std::ifstream f(argv[1]);if(!f)return 1;
        std::string ln; bool inStream=false;
        while(std::getline(f,ln)){
            if(ln.find("<stream>")!=std::string::npos)inStream=true;
            else if(ln.find("</stream>")!=std::string::npos)break;
            else if(inStream)input+=ln+"\n";
        }
    } else { std::ostringstream ss; ss<<std::cin.rdbuf(); input=ss.str(); }

    ProtocolView pv;
    Markdown md; md.setWidth(56);
    std::string buf, body, openName;
    std::map<std::string,std::string> openA;
    int delayMs = 150;  // speed: lower = faster, set via arg or env
    const char* env = getenv("PIPE_SPEED");
    if(env) delayMs = std::stoi(env);
    if(argc>2) delayMs = std::stoi(argv[2]);

    auto render = [&]{ for(auto& l:pv.incremental(60)) std::cout<<"  "<<l<<"\n"; std::cout<<std::flush; };

    for(size_t i=0;i<input.size();i++){
        buf+=input[i];
        bool emit=(input[i]=='>'||input[i]=='\n');
        if(!emit&&i+1<input.size())continue;
        if(emit&&input[i]=='\n'&&buf.size()==1){buf.clear();continue;}
        std::string tok=buf; buf.clear();
        // Trim
        while(!tok.empty()&&(tok[0]==' '||tok[0]=='\n'||tok[0]=='\t'))tok.erase(0,1);
        if(tok.empty())continue;
        // Show raw
        std::string show=tok; if(show.size()>40)show=show.substr(0,37)+"...";
        std::cout<<ansi::dim()<<"← "<<show<<ansi::reset();
        bool done=false;

        // Complete tag?
        if(tok[0]=='<'&&tok.back()=='>'){
            auto t=parse(tok); done=true;
            if(!t.close){ // Opening
                openName=t.name; openA=t.a; body.clear();
                if(t.name=="action"){
                    ActionType at=ActionType::TOOL;
                    if(t.a["type"]=="agent")at=ActionType::AGENT;
                    else if(t.a["type"]=="relic")at=ActionType::RELIC;
                    else if(t.a["type"]=="feed")at=ActionType::FEED;
                    pv.addAction({at,t.a["name"],t.a["id"],t.a["mode"]!="async"});
                }
            }else{ // Closing
                if(t.name=="result"&&!body.empty())
                    pv.addResult({openA["id"],openA["status"]=="ok",body});
                else if(t.name=="response"){
                    md.setText(body);
                    pv.addRenderedLines(md.render());
                }
                else if(t.name=="thought")
                    pv.addContent({BlockType::THOUGHT,body,false});
                openName.clear(); body.clear();
            }
        }
        // Body text (inside open tag or split body+close)
        else if(!openName.empty()){
            size_t ct=tok.find("</");
            if(ct!=std::string::npos&&tok.back()=='>'){
                body+=tok.substr(0,ct); auto ct2=parse(tok.substr(ct)); done=true;
                if(ct2.name=="result"&&!body.empty())
                    pv.addResult({openA["id"],openA["status"]=="ok",body});
                else if(ct2.name=="response"){
                    md.setText(body);
                    pv.addRenderedLines(md.render());
                } else if(ct2.name=="thought"){
                    pv.addContent({BlockType::THOUGHT,body,false});
                }
                openName.clear(); body.clear();
            }else if(openName!="action"){ body+=tok; done=true; }
        }
        // Plain text outside any tag
        else if(!tok.empty()){ md.setText(tok); pv.addRenderedLines(md.render()); done=true; }

        if(!done)std::cout<<" ?\n"; else render();
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    // Flush remaining buffer
    if(!buf.empty()){ std::string tok=buf; buf.clear();
        while(!tok.empty()&&(tok[0]==' '||tok[0]=='\n'||tok[0]=='\t'))tok.erase(0,1);
        if(!tok.empty()&&tok!="\n"){ md.setText(tok); pv.addRenderedLines(md.render()); }
        for(auto& l:pv.incremental(60)) std::cout<<"  "<<l<<"\n";
    }
    std::cout<<"\n─── done ───\n";
}

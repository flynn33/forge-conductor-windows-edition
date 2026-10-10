#pragma once
#include <string_view>

namespace ForgeConductor::NativeTools::Windows::ComfyDetail {
// Fixed introspection of the selected external provider interpreter. Inputs are
// data in an authorized JSON file; neither Python code nor network access is
// supplied by a tool caller. Packaging semantics come from that interpreter's
// installed pip rather than an approximation of PEP 440/508 in Forge.
inline constexpr std::string_view PackageContracts = R"FORGECOMFY(
import json,sys,importlib.metadata as metadata
from pip._vendor.packaging.requirements import Requirement
from pip._vendor.packaging.version import Version,InvalidVersion
from pip._vendor.packaging.specifiers import SpecifierSet
from pip._vendor.packaging.markers import default_environment
from pip._vendor.packaging.tags import sys_tags
from pip._vendor.packaging.utils import canonicalize_name,parse_wheel_filename,parse_sdist_filename
with open(sys.argv[1],encoding='utf-8') as f: q=json.load(f)
def active(r,extras):
    if r.marker and not any(r.marker.evaluate(dict(default_environment(),extra=x)) for x in (extras or [''])): return False
    if r.url: raise ValueError('Direct URL/VCS Python requirements need separately sealed wheel provenance')
    return True
mode=q['mode']
if mode=='model_paths':
    import ast
    with open(q['path'],encoding='utf-8') as f: tree=ast.parse(f.read())
    result=next((ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='model_dir_name_map' for t in n.targets)),None)
    if not isinstance(result,dict) or not all(isinstance(k,str) and isinstance(v,str) for k,v in result.items()): raise ValueError('Installed publisher model-directory mapping is not a literal string mapping')
elif mode=='requirements_file':
    import os,hashlib
    from pip._internal.req import req_file
    root=os.path.normcase(os.path.realpath(q['root'])); files=[]; total=0
    def local_content(path,session):
        global total
        if path.startswith(('http:','https:','file:')): raise ValueError('Requirement includes must be prepared as identified local publisher files: '+path)
        path=os.path.realpath(path)
        if os.path.commonpath([root,os.path.normcase(path)])!=root: raise ValueError('Requirement include leaves the prepared node source: '+path)
        with open(path,'rb') as f: content=f.read(1048577)
        total+=len(content)
        if len(content)>1048576 or total>4194304 or len(files)>=64: raise ValueError('Included requirements exceed the preparation inventory bound')
        files.append({'path':path,'sha256':hashlib.sha256(content).hexdigest(),'bytes':len(content)})
        return path,req_file._decode_req_file(content,path)
    req_file.get_file_content=local_content
    original_parser=req_file.get_line_parser
    def identified_parser(finder):
        parser=original_parser(finder)
        def line_parser(line):
            args,opts=parser(line)
            if any(getattr(opts,k,None) for k in ('index_url','extra_index_urls','no_index','find_links','prefer_binary','require_hashes','pre','trusted_hosts','features_enabled','config_settings')) or opts.format_control.no_binary or opts.format_control.only_binary or opts.release_control.all_releases or opts.release_control.only_final:
                raise ValueError('Requirement-file installer options need separately resolved publisher/build provenance: '+line)
            return args,opts
        return line_parser
    req_file.get_line_parser=identified_parser
    result={'requirements':[],'constraints':[],'files':files}
    for r in req_file.parse_requirements(q['path'],session=None):
        if r.is_editable: raise ValueError('Editable requirement requires a staged publisher source build: '+r.requirement)
        if r.options.get('hashes'): raise ValueError('Requirement-file hashes require separately sealed package descriptors: '+r.requirement)
        result['constraints' if r.constraint else 'requirements'].append(r.requirement)
elif mode=='setup_requirements':
    import configparser
    config=configparser.ConfigParser(interpolation=None)
    with open(q['path'],encoding='utf-8') as f: config.read_file(f)
    python=config.get('options','python_requires',fallback='')
    if python and not SpecifierSet(python).contains(default_environment()['python_full_version']): raise ValueError('Node setup.cfg requires a different provider Python version: '+python)
    result=[r.strip() for r in config.get('options','install_requires',fallback='').splitlines() if r.strip() and not r.lstrip().startswith('#')]
    for extra in q.get('extras',[]):
        if not config.has_option('options.extras_require',extra): raise ValueError('Requested setup.cfg extra is not declared: '+extra)
        result.extend(r.strip() for r in config.get('options.extras_require',extra).splitlines() if r.strip() and not r.lstrip().startswith('#'))
    if any(r.startswith(('file:','attr:')) for r in result): raise ValueError('Dynamic setup.cfg requirement directives need an identified source build')
    if len(result)>1024 or not all(isinstance(r,str) and len(r)<=4096 for r in result): raise ValueError('Declarative setup requirements exceed the preparation contract bound')
elif mode=='project_requirements':
    try: import tomllib
    except ImportError: from pip._vendor import tomli as tomllib
    with open(q['path'],'rb') as f: project=tomllib.load(f).get('project',{})
    if 'dependencies' in project.get('dynamic',[]): raise ValueError('Dynamic project dependencies require separately identified wheel provenance')
    if project.get('requires-python') and not SpecifierSet(project['requires-python']).contains(default_environment()['python_full_version']):
        raise ValueError('Node project requires a different provider Python version: '+project['requires-python'])
    result=project.get('dependencies',[])
    if not isinstance(result,list) or not all(isinstance(r,str) for r in result): raise ValueError('Project dependencies must be literal requirement strings')
    optional=project.get('optional-dependencies',{})
    for extra in q.get('extras',[]):
        if extra not in optional: raise ValueError('Requested node project extra is not declared: '+extra)
        result.extend(optional[extra])
elif mode=='build_configuration':
    import os
    from pip._internal.pyproject import load_pyproject_toml
    from pip._vendor.pyproject_hooks._impl import norm_and_check
    source=os.path.realpath(q['source']); details=load_pyproject_toml(os.path.join(source,'pyproject.toml'),os.path.join(source,'setup.py'),q['name'])
    paths=[norm_and_check(source,p) for p in details.backend_path]
    if any(os.path.commonpath([source,os.path.realpath(p)])!=source for p in paths): raise ValueError('Source build backend path leaves its prepared project')
    result={'requirements':list(details.requires)+list(details.check),'backend':details.backend,'backend_path':paths}
elif mode in ('build_requires','build_metadata','build_wheel'):
    import os,site
    from pip._vendor.pyproject_hooks._in_process import _in_process as hooks
    source=os.path.realpath(q['source']); os.chdir(source)
    if q.get('overlay'):
        from pip._internal.locations import get_scheme
        overlay=os.path.realpath(q['overlay']); sys.path.insert(0,overlay); site.addsitedir(overlay)
        scripts=os.path.realpath(get_scheme('',home=overlay).scripts)
        if os.path.commonpath([os.path.normcase(overlay),os.path.normcase(scripts)])!=os.path.normcase(overlay): raise ValueError('Build dependency script scheme leaves the inactive overlay')
        os.environ['PYTHONPATH']=os.pathsep.join([overlay]+sys.path)
        os.environ['PATH']=os.pathsep.join([os.path.dirname(sys.executable),scripts,os.path.join(overlay,'Scripts'),os.environ.get('PATH','')])
        os.environ['PYTHONNOUSERSITE']='1'
    os.environ['_PYPROJECT_HOOKS_BUILD_BACKEND']=q['backend']
    if q.get('backend_path'): os.environ['_PYPROJECT_HOOKS_BACKEND_PATH']=os.pathsep.join(q['backend_path'])
    else: os.environ.pop('_PYPROJECT_HOOKS_BACKEND_PATH',None)
    os.environ['PIP_NO_INDEX']='1'; os.environ['PIP_DISABLE_PIP_VERSION_CHECK']='1'
    if mode=='build_requires': result={'requirements':hooks.get_requires_for_build_wheel(None)}
    else:
        output=os.path.realpath(q['output']); os.makedirs(output,exist_ok=True)
        if mode=='build_wheel': filename=hooks.build_wheel(output,None,q.get('metadata_directory')); result={'wheel':os.path.join(output,filename)}
        else:
            try: filename=hooks.prepare_metadata_for_build_wheel(output,None,False); result={'metadata_directory':os.path.join(output,filename)}
            except hooks.HookMissing: result={'requires_wheel':True}
        key='wheel' if mode=='build_wheel' else 'metadata_directory'
        if key in result and (os.path.basename(filename)!=filename or os.path.commonpath([output,os.path.realpath(result[key])])!=output): raise ValueError('Source backend returned an invalid output location')
elif mode in ('wheel_metadata','directory_metadata'):
    import os,zipfile
    from pip._vendor.packaging.metadata import Metadata
    if mode=='wheel_metadata':
        with zipfile.ZipFile(q['path']) as archive:
            files=archive.infolist()
            if len(files)>100000 or sum(f.file_size for f in files)>68719476736: raise ValueError('Built wheel exceeds its installed-content bound')
            metadata_files=[f for f in files if f.filename.endswith('.dist-info/METADATA')]
            if len(metadata_files)!=1 or metadata_files[0].file_size>1048576: raise ValueError('Built wheel lacks exactly one bounded package metadata file')
            raw=archive.read(metadata_files[0]); bad=archive.testzip()
            if bad: raise ValueError('Built wheel has corrupt archive content: '+bad)
    else:
        with open(os.path.join(q['path'],'METADATA'),'rb') as f: raw=f.read(1048577)
        if len(raw)>1048576: raise ValueError('Built package metadata exceeds its bound')
    package=Metadata.from_email(raw,validate=False)
    result={'name':canonicalize_name(package.name),'version':str(package.version),'requires_dist':[str(r) for r in package.requires_dist or []],'requires_python':str(package.requires_python or '')}
)FORGECOMFY"
R"FORGECOMFY(elif mode=='node_project_identity':
    try: import tomllib
    except ImportError: from pip._vendor import tomli as tomllib
    with open(q['path'],'rb') as f: document=tomllib.load(f)
    project=document.get('project',{}); urls=project.get('urls',{}); comfy=document.get('tool',{}).get('comfy',{})
    result={'id':project.get('name',''),'version':project.get('version',''),'repository':urls.get('Repository',''),'publisher_id':comfy.get('PublisherId','')}
    if not all(isinstance(v,str) and len(v)<=4096 for v in result.values()): raise ValueError('Registry source identity must contain bounded literal strings')
    result['id']=result['id'].strip().lower()
elif mode=='version_equal':
    result={'equal':Version(q['actual'])==Version(q['expected'])}
elif mode=='inventory':
    result={'python_version':default_environment()['python_full_version'],'tags':[str(t) for t in sys_tags()],
      'packages':{canonicalize_name(d.metadata['Name']):{'version':d.version,'requires':d.requires or []} for d in metadata.distributions() if d.metadata['Name']}}
elif mode=='requirements':
    result=[]
    for line in q['requirements']:
        r=Requirement(line)
        if active(r,q.get('extras',[])):
            r.marker=None
            result.append({'name':canonicalize_name(r.name),'requirement':str(r),'extras':sorted(r.extras),'specifier':str(r.specifier)})
elif mode=='select':
    specs=[Requirement(s).specifier for s in q['requirements']]
    tags={t:i for i,t in enumerate(q['environment']['tags'])}
    candidates=[]
    for version,files in q['releases'].items():
        try: v=Version(version)
        except InvalidVersion: continue
        if (v.is_prerelease or v.is_devrelease) and not any(s.prereleases for s in specs): continue
        if not all(s.contains(v,prereleases=None) for s in specs): continue
        for f in files:
            if f.get('yanked') or f.get('packagetype')!='bdist_wheel': continue
            if f.get('requires_python') and not SpecifierSet(f['requires_python']).contains(q['environment']['python_version']): continue
            try: name,wv,build,wt=parse_wheel_filename(f['filename'])
            except Exception: continue
            rank=min((tags[str(t)] for t in wt if str(t) in tags),default=999999)
            if canonicalize_name(name)!=q['name'] or rank==999999: continue
            candidates.append((v,-rank,f))
    if not candidates: raise ValueError('No compatible publisher wheel satisfies '+repr(q['requirements']))
    v,rank,f=max(candidates,key=lambda x:(x[0],x[1]))
    result={'name':q['name'],'version':str(v),'filename':f['filename'],'url':f['url'],'sha256':f['digests']['sha256'],'bytes':f['size']}
elif mode=='resolve':
    from pip._vendor.resolvelib import Resolver,AbstractProvider,BaseReporter
    from pip._vendor.resolvelib.resolvers import ResolutionImpossible,ResolutionTooDeep
    environment=q['environment']; baseline=environment['packages']; publishers=q.get('publishers',{}); tags={t:i for i,t in enumerate(environment['tags'])}; wheels={}; constraints={}; isolated_build=q.get('isolated_build',False)
    def parsed(lines,extras=()):
        for line in lines or []:
            r=Requirement(line)
            if active(r,extras):
                r.marker=None
                yield r
    for r in parsed(q.get('constraints',[])):
        if r.extras: raise ValueError('Package constraints cannot request extras: '+str(r))
        constraints.setdefault(canonicalize_name(r.name),[]).append(r)
    if not isolated_build:
        for package in baseline.values():
            for r in parsed(package.get('requires',[])): constraints.setdefault(canonicalize_name(r.name),[]).append(r)
    class NeedMetadata(Exception):
        def __init__(self,name,version=None): self.request={'name':name,**({'version':version} if version else {})}
    class NeedSourceBuild(Exception):
        def __init__(self,name,version,file): self.request={'name':name,'version':version,'filename':file['filename'],'url':file['url'],'sha256':file['digests']['sha256'],'bytes':file['size']}
    class Provider(AbstractProvider):
        def identify(self,requirement_or_candidate):
            return canonicalize_name(requirement_or_candidate.name) if isinstance(requirement_or_candidate,Requirement) else requirement_or_candidate[0]
        def get_preference(self,identifier,resolutions,candidates,information,backtrack_causes): return (identifier not in baseline,identifier)
        def is_satisfied_by(self,requirement,candidate):
            return self.identify(requirement)==candidate[0] and requirement.specifier.contains(candidate[1],prereleases=True) and requirement.extras.issubset(candidate[2])
        def find_matches(self,identifier,requirements,incompatibilities):
            requested=list(requirements[identifier]); required=requested+constraints.get(identifier,[]); extras=tuple(sorted(set().union(*(r.extras for r in requested)))); incompatible=set(incompatibilities[identifier])
            if identifier in baseline:
                candidate=(identifier,baseline[identifier]['version'],extras)
                if candidate not in incompatible and all(self.is_satisfied_by(r,candidate) for r in required): return [candidate]
                if not isolated_build: return []
            publisher=publishers.get(identifier,{})
            if 'index' not in publisher: raise NeedMetadata(identifier)
            candidates=[]
            for version,files in publisher['index']['releases'].items():
                try: v=Version(version)
                except InvalidVersion: continue
                if (v.is_prerelease or v.is_devrelease) and not any(r.specifier.prereleases for r in required): continue
                if not all(r.specifier.contains(v,prereleases=None) for r in required): continue
                compatible=[]; sources=[]
                for f in files:
                    if f.get('yanked'): continue
                    if f.get('requires_python') and not SpecifierSet(f['requires_python']).contains(environment['python_version']): continue
                    if f.get('packagetype')=='sdist':
                        try: name,sv=parse_sdist_filename(f['filename'])
                        except Exception: continue
                        if canonicalize_name(name)==identifier and sv==v: sources.append(f)
                        continue
                    if f.get('packagetype')!='bdist_wheel': continue
                    try: name,wv,build,wt=parse_wheel_filename(f['filename'])
                    except Exception: continue
                    rank=min((tags[str(t)] for t in wt if str(t) in tags),default=999999)
                    if canonicalize_name(name)!=identifier or wv!=v or rank==999999: continue
                    compatible.append((rank,f['filename'],f))
                built=publisher.get('built',{}).get(str(v))
                if built:
                    name,wv,build,wt=parse_wheel_filename(built['filename']); rank=min((tags[str(t)] for t in wt if str(t) in tags),default=999999)
                    if canonicalize_name(name)!=identifier or wv!=v or rank==999999 or (built.get('requires_python') and not SpecifierSet(built['requires_python']).contains(environment['python_version'])): continue
                    f=built
                elif compatible: rank,filename,f=min(compatible,key=lambda x:(x[0],x[1]))
                elif sources: rank=999998; f=min(sources,key=lambda entry:entry['filename'])
                else: continue
                candidate=(identifier,str(v),extras)
                if candidate in incompatible: continue
                wheels[(identifier,str(v))]=f
                candidates.append((v,-rank,candidate))
            return [entry[2] for entry in sorted(candidates,key=lambda entry:(entry[0],entry[1]),reverse=True)]
        def get_dependencies(self,candidate):
            name,version,extras=candidate
            if name in baseline and version==baseline[name]['version']: return list(parsed(baseline[name].get('requires',[]),extras))
            versions=publishers[name].get('versions',{})
            if version not in versions: raise NeedMetadata(name,version)
            file=wheels[(name,version)]
            if file.get('packagetype')=='sdist': raise NeedSourceBuild(name,version,file)
            if file.get('local_wheel'): return list(parsed(file.get('requires_dist',[]),extras))
            info=versions[version]['info']
            if canonicalize_name(info['name'])!=name or Version(info['version'])!=Version(version): raise ValueError('Publisher version metadata does not match its requested package pin')
            return list(parsed(info.get('requires_dist',[]),extras))
    try:
        resolved=Resolver(Provider(),BaseReporter()).resolve(list(parsed(q['requirements'])),max_rounds=2048); selected={}; versions={name:p['version'] for name,p in baseline.items()}
        for name,candidate in resolved.mapping.items():
            if name in baseline and candidate[1]==baseline[name]['version']: continue
            version=candidate[1]; f=wheels[(name,version)]; versions[name]=version
            selected[name]={'name':name,'version':version,'filename':f['filename'],'url':f['url'],'sha256':f['digests']['sha256'],'bytes':f['size'],'requirements':f.get('requires_dist',publishers[name]['versions'][version]['info'].get('requires_dist',[])) or [],**({'local_wheel':f['local_wheel']} if f.get('local_wheel') else {})}
        if len(selected)>128: raise ValueError('Package resolution exceeds 128 new wheels')
        result={'selected':selected,'versions':versions,'requirements':sorted({str(i.requirement) for criterion in resolved.criteria.values() for i in criterion.information})}
    except NeedMetadata as need: result={'need':need.request}
    except NeedSourceBuild as need: result={'build':need.request}
    except ResolutionImpossible as error:
        result={'error':'conflict','conflicts':[{'requirement':str(c.requirement),'parent':None if c.parent is None else c.parent[0]+'=='+c.parent[1]} for c in list(error.causes)[:64]]}
    except ResolutionTooDeep: result={'error':'resolution_limit','rounds':2048}
elif mode=='check':
    errors=[]
    for line in q['requirements']:
        r=Requirement(line)
        if not active(r,q.get('extras',[])): continue
        name=canonicalize_name(r.name);version=q['versions'].get(name)
        if not version or not r.specifier.contains(version,prereleases=True): errors.append(str(r)+' (selected '+str(version)+')')
    result={'compatible':not errors,'conflicts':errors}
else: raise ValueError('Unknown fixed package contract operation')
if q.get('result_path'):
    with open(q['result_path'],'w',encoding='utf-8') as f: json.dump(result,f,separators=(',',':'))
else: print(json.dumps(result,separators=(',',':')))
)FORGECOMFY";
}

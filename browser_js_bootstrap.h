#pragma once

// Web-facing objects remain JavaScript objects so expandos, property descriptors
// and event callbacks have ordinary ECMAScript identity and lifetime semantics.
static const char browser_js_bootstrap[] = R"JS(
(function(){
'use strict';
// Browser legacy RegExp statics are used by older AMD loaders. Preserve the
// engine's actual match result rather than rewriting URLs or page programs.
const nativeExec=RegExp.prototype.exec;let legacyMatch=null,legacyInput='';
RegExp.prototype.exec=function(input){const value=String(input);const result=nativeExec.call(this,value);if(result){legacyMatch=result.slice();legacyMatch.index=result.index;legacyInput=value}return result};
RegExp.prototype.test=function(input){return this.exec(input)!==null};
for(let i=1;i<=9;i++)Object.defineProperty(RegExp,'$'+i,{configurable:true,get:()=>legacyMatch?legacyMatch[i]||'':''});
for(const [name,get] of Object.entries({lastMatch:()=>legacyMatch?legacyMatch[0]:'',lastParen:()=>legacyMatch?legacyMatch.slice(1).filter(x=>x!==undefined).slice(-1)[0]||'':'',leftContext:()=>legacyMatch?legacyInput.slice(0,legacyMatch.index):'',rightContext:()=>legacyMatch?legacyInput.slice(legacyMatch.index+legacyMatch[0].length):'',input:()=>legacyInput}))Object.defineProperty(RegExp,name,{configurable:true,get});
for(const [name,alias] of [['$&','lastMatch'],['$+','lastParen'],['$`','leftContext'],["$'",'rightContext'],['$_','input']])Object.defineProperty(RegExp,name,{configurable:true,get:()=>RegExp[alias]});
const listeners=Symbol('browser event listeners'), trustedEvents=new WeakSet();
const b=globalThis.__browser;
const htmlNamespace='http://www.w3.org/1999/xhtml',svgNamespace='http://www.w3.org/2000/svg',mathNamespace='http://www.w3.org/1998/Math/MathML';
const nodeNamespace=Symbol('DOM namespace'),nodeLocalName=Symbol('DOM local name'),templateContent=Symbol('template content');
class DOMException extends Error {
 constructor(message='',name='Error'){super(String(message));this.name=String(name);this.code=({IndexSizeError:1,HierarchyRequestError:3,InvalidCharacterError:5,NotFoundError:8,NotSupportedError:9,InvalidStateError:11,SyntaxError:12,NamespaceError:14,SecurityError:18,QuotaExceededError:22,DataCloneError:25})[this.name]||0}
}
function nativeChildren(n){return list(b('children',n._id))}
function nativeLocalName(n){const name=String(b('get',n._id,'nodeName')).toLowerCase();return ({foreignobject:'foreignObject',lineargradient:'linearGradient',radialgradient:'radialGradient',clippath:'clipPath',textpath:'textPath'})[name]||name}
function namespaceOf(n){if(Object.prototype.hasOwnProperty.call(n,nodeNamespace))return n[nodeNamespace];const name=nativeLocalName(n);if(name==='svg')return svgNamespace;if(name==='math')return mathNamespace;const parent=n.parentElement;if(parent&&parent.namespaceURI!==htmlNamespace&&!(parent.namespaceURI===svgNamespace&&parent.localName==='foreignObject'))return parent.namespaceURI;return htmlNamespace}
function setNamespace(n,namespace,name){Object.defineProperty(n,nodeNamespace,{value:namespace,configurable:true});Object.defineProperty(n,nodeLocalName,{value:name.includes(':')?name.slice(name.indexOf(':')+1):name,configurable:true});Object.setPrototypeOf(n,namespace===svgNamespace?SVGElement.prototype:namespace===mathNamespace?MathMLElement.prototype:namespace===htmlNamespace?(name.toLowerCase()==='template'?HTMLTemplateElement.prototype:name.toLowerCase()==='a'||name.toLowerCase()==='area'?HTMLAnchorElement.prototype:name.toLowerCase()==='link'?HTMLLinkElement.prototype:HTMLElement.prototype):Element.prototype);return n}
function cloneMetadata(source,copy,deep){const pending=[[source,copy]];let count=0;while(pending.length){if(++count>50000)throw new DOMException('DOM clone metadata budget exceeded','QuotaExceededError');const [a,c]=pending.pop();if(a.nodeType===1)setNamespace(c,a.namespaceURI,a.localName);if(!deep)continue;if(a instanceof HTMLTemplateElement){const fragment=node(b('clone',a.content._id,true));Object.defineProperty(c,templateContent,{value:fragment});pending.push([a.content,fragment])}const aa=nativeChildren(a),cc=nativeChildren(c);for(let i=0;i<aa.length&&i<cc.length;i++)pending.push([aa[i],cc[i]])}return copy}
function initializeTemplates(container){for(const template of list(b('query-all',container._id,'template')))if(template instanceof HTMLTemplateElement)void template.content}
function checkInsertion(parent,child,reference){if(!(child instanceof Node))throw TypeError('Inserted value is not a Node');if(reference!==null&&reference!==undefined&&(!(reference instanceof Node)||reference.parentNode!==parent))throw new DOMException('Reference is not a child','NotFoundError');if(child===parent||child.contains(parent))throw new DOMException('Cyclic DOM insertion','HierarchyRequestError')}

function enqueueMicrotask(callback){return b('enqueue-microtask',0,callback)}
function listItem(index){return this[index]||null}
function listNamedItem(name){return this.find(value=>value.id===name||value.name===name)||null}
function list(ids){const a=ids.map(node);a.item=listItem;a.namedItem=listNamedItem;return a;}
function attr(n,k,v){if(arguments.length===3){const old=b('attr',n._id,k);b('attr-set',n._id,k,String(v));notify(n,'attributes',[],[],k,old);return;}return b('attr',n._id,k);}
function cssName(n){return String(n).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());}
function style(n){return new Proxy({getPropertyValue(k){return this[cssName(k)]||''},setProperty(k,v){this[cssName(k)]=v},removeProperty(k){const v=this[k];this[k]='';return v}}, {
 get(t,k){if(k in t)return t[k];const text=attr(n,'style')||'';if(k==='cssText')return text;const name=cssName(k);for(const d of text.split(';')){const i=d.indexOf(':');if(i>=0&&d.slice(0,i).trim()===name)return d.slice(i+1).trim()}return ''},
 set(t,k,v){if(k==='cssText'){attr(n,'style',v);return true}const name=cssName(k);let text=(attr(n,'style')||'').split(';').filter(d=>d.slice(0,d.indexOf(':')).trim()!==name&&d.trim()).join(';');if(v!=='')text+=';'+name+':'+v;attr(n,'style',text);return true}
});}
class Event {
 constructor(type,options={}){this.type=String(type);this.bubbles=!!options.bubbles;this.cancelable=!!options.cancelable;this.defaultPrevented=false;Object.defineProperty(this,'isTrusted',{enumerable:true,get:()=>trustedEvents.has(this)});this.target=null;this.currentTarget=null;this.eventPhase=0;this.timeStamp=b('now');Object.assign(this,options)}
 get returnValue(){return !this.defaultPrevented} set returnValue(value){if(value===false)this.preventDefault()}
 get cancelBubble(){return !!this._stop} set cancelBubble(value){if(value)this.stopPropagation()}
 preventDefault(){if(this.cancelable)this.defaultPrevented=true}
 stopPropagation(){this._stop=true}
 stopImmediatePropagation(){this._stop=true;this._immediate=true}
 initEvent(type,bubbles,cancelable){this.type=type;this.bubbles=bubbles;this.cancelable=cancelable}
}
function listenerState(target,create=false){
 if(target instanceof Node&&target._id){let state=b('listeners',target._id);if(!state&&create){state=new Map();b('listeners',target._id,state)}return state}
 let state=Object.getOwnPropertyDescriptor(target,listeners);if(!state&&create){Object.defineProperty(target,listeners,{value:new Map()});state=Object.getOwnPropertyDescriptor(target,listeners)}return state?state.value:null;
}
function add(type,fn,options){if(!fn)return;let map=listenerState(this,true);let a=map.get(type);if(!a)map.set(type,a=[]);const capture=options===true||!!(options&&options.capture);if(!a.some(x=>x.fn===fn&&x.capture===capture))a.push({fn,capture,once:!!(options&&options.once)})}
function remove(type,fn,options){const map=listenerState(this);if(!map)return;const a=map.get(type)||[],capture=options===true||!!(options&&options.capture);map.set(type,a.filter(x=>x.fn!==fn||x.capture!==capture))}
function reportEventError(error,kind='event listener'){b('log',0,'Uncaught '+kind+' exception: '+String(error)+(error&&error.stack?'\n'+error.stack:''))}
function invoke(target,event,capture){
 event.currentTarget=target;const a=(listenerState(target)||new Map()).get(event.type)||[];
 for(const item of a.slice()){
  if(item.capture!==capture)continue;if(item.once)remove.call(target,event.type,item.fn,item.capture);
  try{if(typeof item.fn==='function')item.fn.call(target,event);else if(item.fn&&item.fn.handleEvent)item.fn.handleEvent(event)}catch(error){reportEventError(error)}
  if(event._immediate)break;
 }
 if(!capture&&!event._immediate){try{let fn=target['on'+event.type];if(typeof fn!=='function'&&target.getAttribute){const code=target.getAttribute('on'+event.type);if(code)fn=Function('event',code)}if(typeof fn==='function'&&fn.call(target,event)===false)event.preventDefault()}catch(error){reportEventError(error)}}
}
function dispatch(event,trusted=false){if(!(event instanceof Event))event=Object.assign(new Event(event.type),event);if(trusted)trustedEvents.add(event);else trustedEvents.delete(event);event.target=this;const path=[this];let n=this;while(n&&n!==globalThis){n=n===document?globalThis:n.parentNode;if(n)path.push(n)}for(let i=path.length-1;i>0&&!event._stop;i--){event.eventPhase=1;invoke(path[i],event,true)}if(!event._stop){event.eventPhase=2;invoke(this,event,true);if(!event._immediate)invoke(this,event,false)}if(event.bubbles)for(let i=1;i<path.length&&!event._stop;i++){event.eventPhase=3;invoke(path[i],event,false)}event.eventPhase=0;return !event.defaultPrevented}
class Node {
 constructor(id){Object.defineProperty(this,'_id',{value:id})}
 appendChild(child){checkInsertion(this,child,null);if(child.nodeType===11){for(const n of child.childNodes)this.appendChild(n);return child}const old=child.parentNode;b('append',this._id,child._id);if(old)notify(old,'childList',[],[child]);notify(this,'childList',[child],[]);return child}
 removeChild(child){if(!(child instanceof Node)||child.parentNode!==this)throw new DOMException('Node is not a child','NotFoundError');b('remove',this._id,child._id);notify(this,'childList',[],[child]);return child}
 insertBefore(child,reference){checkInsertion(this,child,reference);if(child===reference)return child;if(child.nodeType===11){for(const n of child.childNodes)this.insertBefore(n,reference);return child}const old=child.parentNode;b('insert',this._id,child._id,reference?reference._id:0);if(old)notify(old,'childList',[],[child]);notify(this,'childList',[child],[]);return child}
 replaceChild(child,old){this.insertBefore(child,old);this.removeChild(old);return old}
 remove(){if(this.parentNode)this.parentNode.removeChild(this)}
 hasChildNodes(){return this.childNodes.length!==0}
 contains(child){for(let x=child;x;x=x.parentNode)if(x===this)return true;return false}
 cloneNode(deep=false){return cloneMetadata(this,node(b('clone',this._id,!!deep)),!!deep)}
 addEventListener(...args){return add.apply(this,args)} removeEventListener(...args){return remove.apply(this,args)} dispatchEvent(event){return dispatch.call(this,event)}
 get nodeType(){return +b('get',this._id,'nodeType')}
 get nodeName(){return b('get',this._id,'nodeName')}
 get namespaceURI(){return null} get prefix(){return null}
 getRootNode(options={}){let n=this;while(n.parentNode)n=n.parentNode;return n}
 get ownerDocument(){return document}
 get parentNode(){return node(b('parent',this._id))||(this===document.documentElement?document:null)}
 get parentElement(){const p=this.parentNode;return p&&p.nodeType===1?p:null}
 get childNodes(){return list(b('children',this._id))}
 get children(){return this.childNodes.filter(n=>n.nodeType===1)}
 get firstChild(){return node(b('relative',this._id,'firstChild'))} get lastChild(){return node(b('relative',this._id,'lastChild'))}
 get firstElementChild(){return node(b('relative',this._id,'firstElementChild'))} get lastElementChild(){return node(b('relative',this._id,'lastElementChild'))}
 get nextSibling(){return node(b('relative',this._id,'nextSibling'))}
 get previousSibling(){return node(b('relative',this._id,'previousSibling'))}
 get nextElementSibling(){return node(b('relative',this._id,'nextElementSibling'))}
 get previousElementSibling(){return node(b('relative',this._id,'previousElementSibling'))}
 get textContent(){return b('text',this._id)} set textContent(value){const old=this.textContent,removed=this.childNodes;b('text-set',this._id,String(value??''));notify(this,this.nodeType===3||this.nodeType===8?'characterData':'childList',this.childNodes,removed,null,old)}
 get data(){return this.textContent} set data(value){this.textContent=String(value)}
 get length(){return this.textContent.length}
 appendData(value){this.data=this.data+value} insertData(offset,value){this.data=this.data.slice(0,offset)+value+this.data.slice(offset)}
 deleteData(offset,count){this.data=this.data.slice(0,offset)+this.data.slice(offset+count)}
 replaceData(offset,count,value){this.data=this.data.slice(0,offset)+value+this.data.slice(offset+count)}
 substringData(offset,count){return this.data.slice(offset,offset+count)}
 splitText(offset){if(this.nodeType!==3)throw Error('InvalidNodeTypeError');const next=document.createTextNode(this.data.slice(offset));this.data=this.data.slice(0,offset);if(this.parentNode)this.parentNode.insertBefore(next,this.nextSibling);return next}
 get nodeValue(){return this.nodeType===3||this.nodeType===8?this.textContent:null} set nodeValue(v){this.textContent=v}
}
Object.assign(Node,{ELEMENT_NODE:1,ATTRIBUTE_NODE:2,TEXT_NODE:3,CDATA_SECTION_NODE:4,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_FRAGMENT_NODE:11});Object.assign(Node.prototype,Node);
class Element extends Node {
 get tagName(){return b('get',this._id,'nodeName')}
 get namespaceURI(){return namespaceOf(this)} get localName(){return this[nodeLocalName]||nativeLocalName(this)}
 get prefix(){const name=String(b('get',this._id,'nodeName'));return name.includes(':')?name.slice(0,name.indexOf(':')).toLowerCase():null}
 getAttribute(name){return attr(this,String(name).toLowerCase())}
 getAttributeNames(){return b('attr-names',this._id)}
 getAttributeNode(name){const value=this.getAttribute(name);return value===null?null:{name:String(name),value,nodeValue:value,specified:true}}
 setAttributeNode(attribute){const previous=this.getAttributeNode(attribute.name);this.setAttribute(attribute.name,attribute.value);return previous}
 removeAttributeNode(attribute){this.removeAttribute(attribute.name);return attribute}
 get attributes(){const a=b('attr-names',this._id).map(name=>this.getAttributeNode(name));for(const n of a)a[n.name]=n;a.getNamedItem=name=>a[name]||null;a.item=i=>a[i]||null;return a}
 setAttribute(name,value){attr(this,String(name).toLowerCase(),value)}
 removeAttribute(name){name=String(name).toLowerCase();const old=this.getAttribute(name);b('attr-remove',this._id,name);if(old!==null)notify(this,'attributes',[],[],name,old)}
 hasAttribute(name){return this.getAttribute(name)!==null} hasAttributes(){return this.attributes.length>0}
 getAttributeNS(ns,name){return this.getAttribute(namespaceAttribute(ns,name))} setAttributeNS(ns,name,value){this.setAttribute(name,value)}
 removeAttributeNS(ns,name){this.removeAttribute(namespaceAttribute(ns,name))}
 toggleAttribute(name,force){const on=force===undefined?!this.hasAttribute(name):!!force;on?this.setAttribute(name,''):this.removeAttribute(name);return on}
 get id(){return this.getAttribute('id')||''} set id(v){this.setAttribute('id',v)}
 get className(){return this.getAttribute('class')||''} set className(v){this.setAttribute('class',v)}
 get classList(){const self=this;return {contains(x){return self.className.split(/\s+/).includes(x)},add(...xs){self.className=Array.from(new Set(self.className.split(/\s+/).filter(Boolean).concat(xs))).join(' ')},remove(...xs){self.className=self.className.split(/\s+/).filter(x=>!xs.includes(x)).join(' ')},toggle(x,force){const present=this.contains(x),on=force===undefined?!present:!!force;on?this.add(x):this.remove(x);return on},toString(){return self.className}}}
 get style(){return this._style||(this._style=style(this))} set style(v){this.style.cssText=v}
 get dataset(){const self=this;return new Proxy({}, {get(t,k){return self.getAttribute('data-'+cssName(k))},set(t,k,v){self.setAttribute('data-'+cssName(k),v);return true}})}
 get innerHTML(){return b('html',this._id)} set innerHTML(v){const removed=this.childNodes;b('html-set',this._id,String(v));initializeTemplates(this);notify(this,'childList',this.childNodes,removed)}
 get outerHTML(){return b('outer',this._id)}
 get innerText(){return this.textContent} set innerText(v){this.textContent=v}
 querySelector(s){return node(b('query',this._id,String(s)))} querySelectorAll(s){return list(b('query-all',this._id,String(s)))}
 getElementsByTagName(s){return this.querySelectorAll(s)} getElementsByClassName(s){return this.querySelectorAll('.'+String(s).trim().split(/\s+/).join('.'))}
 matches(s){return !!b('matches',this._id,String(s))} closest(s){for(let n=this;n;n=n.parentElement)if(n.matches(s))return n;return null}
 insertAdjacentHTML(where,html){const holder=document.createElement('div');holder.innerHTML=html;for(const child of holder.childNodes){if(where==='beforeend')this.appendChild(child);else if(where==='afterbegin')this.insertBefore(child,this.firstChild);else if(where==='beforebegin')this.parentNode.insertBefore(child,this);else this.parentNode.insertBefore(child,this.nextSibling)}}
 get value(){return b('control-get',this._id,'value')} set value(v){b('control-set',this._id,'value',String(v))}
 get checked(){return b('control-get',this._id,'checked')==='true'} set checked(v){b('control-set',this._id,'checked',String(!!v))}
 get selected(){return b('control-get',this._id,'selected')==='true'} set selected(v){b('control-set',this._id,'selected',String(!!v))}
 get disabled(){return this.hasAttribute('disabled')} set disabled(v){v?this.setAttribute('disabled',''):this.removeAttribute('disabled')}
 get readOnly(){return this.hasAttribute('readonly')} set readOnly(v){v?this.setAttribute('readonly',''):this.removeAttribute('readonly')}
 get form(){for(let n=this.parentElement;n;n=n.parentElement)if(n.tagName==='FORM')return n;const id=this.getAttribute('form');return id?document.getElementById(id):null}
 get elements(){const a=this.querySelectorAll('input,textarea,select,button');for(const e of a){if(e.name&&!Object.prototype.hasOwnProperty.call(a,e.name))a[e.name]=e;if(e.id&&!Object.prototype.hasOwnProperty.call(a,e.id))a[e.id]=e}return a}
 get options(){return this.querySelectorAll('option')}
 focus(){const target=node(this._id);if(!(target instanceof Element))throw TypeError('Invalid focus receiver');const changed=document.activeElement!==target;document.activeElement=target;b('focus',target._id);if(changed)dispatch.call(target,new Event('focus',{bubbles:false}))}
 blur(){document.activeElement=document.body;this.dispatchEvent(new Event('blur',{bubbles:false}))}
 select(){this.focus()}
 submit(){b('submit',this._id)} requestSubmit(){if(this.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true})))this.submit()}
 click(){if(this.dispatchEvent(new Event('click',{bubbles:true,cancelable:true,button:0,buttons:0,which:1,detail:1})))b('click',this._id)}
 getBoundingClientRect(){const r=b('rect',this._id);return Object.assign(r,{right:r.x+r.width,bottom:r.y+r.height,left:r.x,top:r.y})}
 getClientRects(){const rect=this.getBoundingClientRect();return rect.width||rect.height?[rect]:[]}
 get offsetWidth(){return this.getBoundingClientRect().width} get offsetHeight(){return this.getBoundingClientRect().height}
 get clientWidth(){return this.offsetWidth} get clientHeight(){return this.offsetHeight}
 get offsetLeft(){return this.getBoundingClientRect().x+viewport().scrollX} get offsetTop(){return this.getBoundingClientRect().y+viewport().scrollY}
 get scrollWidth(){return this.offsetWidth} get scrollHeight(){return this.offsetHeight}
 get isConnected(){return this===document.documentElement||document.documentElement.contains(this)}
}
for(const key of ['name','type','action','method','src','href','target','title','placeholder','autocomplete','rel','charset','content','httpEquiv','htmlFor','alt','width','height','tabIndex'])Object.defineProperty(Element.prototype,key,{configurable:true,get(){const value=this.getAttribute(key.replace(/[A-Z]/g,c=>'-'+c.toLowerCase()))||'';return ['src','href','action'].includes(key)&&value?b('resolve',0,value):value},set(v){this.setAttribute(key.replace(/[A-Z]/g,c=>'-'+c.toLowerCase()),v)}});
function namespaceAttribute(ns,name){name=String(name);return ns==='http://www.w3.org/1999/xlink'?'xlink:'+name:ns==='http://www.w3.org/XML/1998/namespace'?'xml:'+name:name}
class HTMLElement extends Element {}
class SVGElement extends Element {get tagName(){return this.localName}get nodeName(){return this.localName}}
class MathMLElement extends Element {get tagName(){return this.localName}get nodeName(){return this.localName}}
class DocumentFragment extends Node {
 querySelector(s){return Element.prototype.querySelector.call(this,s)} querySelectorAll(s){return Element.prototype.querySelectorAll.call(this,s)}
 getElementById(id){return this.querySelectorAll('[id]').find(n=>n.id===String(id))||null}
}
class HTMLTemplateElement extends HTMLElement {
 get content(){if(!Object.prototype.hasOwnProperty.call(this,templateContent)){const fragment=document.createDocumentFragment();Object.defineProperty(this,templateContent,{value:fragment});for(const child of nativeChildren(this))fragment.appendChild(child)}return this[templateContent]}
 get innerHTML(){return b('html',this.content._id)} set innerHTML(value){const fragment=this.content,removed=fragment.childNodes;b('html-set',fragment._id,String(value));initializeTemplates(fragment);notify(fragment,'childList',fragment.childNodes,removed)}
 get outerHTML(){const source=b('outer',this._id);return source.slice(0,source.indexOf('>')+1)+this.innerHTML+'</template>'}
}
function tokenList(element,attribute){const tokens=()=>String(element.getAttribute(attribute)||'').trim().split(/\s+/).filter(Boolean);const validate=value=>{value=String(value);if(!value)throw new DOMException('Empty token','SyntaxError');if(/\s/.test(value))throw new DOMException('Whitespace in token','InvalidCharacterError');return value};return {get length(){return tokens().length},get value(){return tokens().join(' ')},set value(value){element.setAttribute(attribute,String(value))},item(index){return tokens()[index]??null},contains(value){return tokens().includes(validate(value))},add(...values){element.setAttribute(attribute,Array.from(new Set(tokens().concat(values.map(validate)))).join(' '))},remove(...values){values=values.map(validate);element.setAttribute(attribute,tokens().filter(t=>!values.includes(t)).join(' '))},toggle(value,force){value=validate(value);const on=force===undefined?!this.contains(value):!!force;on?this.add(value):this.remove(value);return on},replace(oldValue,newValue){oldValue=validate(oldValue);newValue=validate(newValue);const values=tokens(),index=values.indexOf(oldValue);if(index<0)return false;values[index]=newValue;element.setAttribute(attribute,Array.from(new Set(values)).join(' '));return true},supports(value){value=validate(value).toLowerCase();return attribute==='rel'&&!!b('link-supports',element._id,value)},toString(){return this.value},*[Symbol.iterator](){yield*tokens()}}}
class HTMLLinkElement extends HTMLElement {
 get relList(){return this._relList||(this._relList=tokenList(this,'rel'))}
}
for(const [property,attribute]of [['crossOrigin','crossorigin'],['referrerPolicy','referrerpolicy'],['integrity','integrity'],['as','as'],['nonce','nonce']])Object.defineProperty(HTMLLinkElement.prototype,property,{configurable:true,get(){return this.getAttribute(attribute)||''},set(value){this.setAttribute(attribute,value)}});
class HTMLAnchorElement extends HTMLElement {
 get href(){const value=this.getAttribute('href');return value===null?'':b('resolve',0,value)}
 set href(value){this.setAttribute('href',String(value))}
 toString(){return this.href}
}
for(const key of ['protocol','host','hostname','port','pathname','search','hash','origin'])Object.defineProperty(HTMLAnchorElement.prototype,key,{configurable:true,get(){return this.href?parts(this.href)[key]:''},...(key==='origin'?{}:{set(value){const p=parts(this.href||b('url'));value=String(value);if(key==='protocol')p.protocol=value.endsWith(':')?value:value+':';else if(key==='hostname')p.host=value+(p.port?':'+p.port:'');else if(key==='port')p.host=p.hostname+(value?':'+value:'');else if(key==='host')p.host=value;else if(key==='pathname')p.pathname=value.startsWith('/')?value:'/'+value;else if(key==='search')p.search=value&&!value.startsWith('?')?'?'+value:value;else if(key==='hash')p.hash=value&&!value.startsWith('#')?'#'+value:value;this.href=p.protocol+'//'+p.host+p.pathname+p.search+p.hash}})});
function node(id){if(!id)return null;const n=b('wrap',id);if(!n)return null;if(!Object.prototype.hasOwnProperty.call(n,'_id')){Object.defineProperty(n,'_id',{value:id});const type=+b('get',id,'nodeType');Object.setPrototypeOf(n,type===1?HTMLElement.prototype:type===11?DocumentFragment.prototype:Node.prototype);if(type===1){const ns=namespaceOf(n),name=nativeLocalName(n);Object.setPrototypeOf(n,ns===svgNamespace?SVGElement.prototype:ns===mathNamespace?MathMLElement.prototype:name==='template'?HTMLTemplateElement.prototype:name==='a'||name==='area'?HTMLAnchorElement.prototype:name==='link'?HTMLLinkElement.prototype:HTMLElement.prototype)}}return n}
class Document extends Node {
 constructor(){super(0);this.readyState='loading';this.activeElement=null;this.currentScript=null;this.compatMode='CSS1Compat';this.characterSet='UTF-8';this.charset='UTF-8';this.contentType='text/html';this.visibilityState='visible';this.hidden=false}
 get defaultView(){return globalThis} get scrollingElement(){return this.documentElement}
 get nodeType(){return 9} get nodeName(){return '#document'} get parentNode(){return null} get documentElement(){return node(b('root'))} get body(){return this.querySelector('body')} get head(){return this.querySelector('head')}
 get childNodes(){return [this.documentElement]} get ownerDocument(){return null}
 get firstChild(){return this.documentElement} get lastChild(){return this.documentElement}
 get firstElementChild(){return this.documentElement} get lastElementChild(){return this.documentElement}
 querySelector(s){return node(b('query',0,String(s)))} querySelectorAll(s){return list(b('query-all',0,String(s)))}
 getElementsByTagName(s){return this.querySelectorAll(s)} getElementsByClassName(s){return this.querySelectorAll('.'+String(s).trim().split(/\s+/).join('.'))}
 getElementById(id){return node(b('id',0,String(id)))} getElementsByName(name){return list(b('name',0,String(name)))}
 createElement(tag){tag=String(tag).toLowerCase();const created=setNamespace(node(b('create',0,tag)),htmlNamespace,tag);if(created instanceof HTMLTemplateElement)void created.content;return created} createElementNS(ns,tag){ns=ns==null||ns===''?null:String(ns);tag=String(tag);if(tag.includes(':')&&!ns)throw new DOMException('Prefix requires namespace','NamespaceError');const created=setNamespace(node(b('create',0,tag.toLowerCase())),ns,tag);if(created instanceof HTMLTemplateElement)void created.content;return created} createTextNode(text){return node(b('create-text',0,String(text)))} createDocumentFragment(){return node(b('create',0,'xj-fragment'))}
 createComment(text){return node(b('create-comment',0,String(text)))} createAttribute(name){return {name:String(name),value:'',nodeValue:'',specified:true}}
 createRange(){return new Range()} getSelection(){return selection}
 createEvent(){return new Event('')}
 get title(){const e=this.querySelector('title');return e?e.textContent:''} set title(v){let e=this.querySelector('title');if(!e){e=this.createElement('title');this.head.appendChild(e)}e.textContent=v;b('title',0,String(v))}
 get URL(){return b('url')} get documentURI(){return this.URL} get baseURI(){return b('resolve',0,'')} get domain(){return location.hostname}
 get cookie(){return b('cookie')} set cookie(v){b('cookie-set',0,String(v))}
 get referrer(){return ''} get location(){return location} set location(v){location.href=v}
 get forms(){return this.querySelectorAll('form')} get scripts(){return this.querySelectorAll('script')} get images(){return this.querySelectorAll('img')} get links(){return this.querySelectorAll('a[href]')}
 write(...values){const old=this.body?this.body.childNodes.length:0;b('write',0,values.join(''));if(this.body)notify(this.body,'childList',this.body.childNodes.slice(old),[])} writeln(...values){this.write(values.join('')+'\n')} open(){return this} close(){}
}
// Web IDL interface brands keep DOM host objects distinct from plain objects.
// Store the descriptor on each implemented interface, not on individual nodes.
for(const constructor of [Node,Element,HTMLElement,SVGElement,MathMLElement,DocumentFragment,HTMLTemplateElement,HTMLLinkElement,HTMLAnchorElement,Document])Object.defineProperty(constructor.prototype,Symbol.toStringTag,{value:constructor.name,configurable:true});
const document=new Document();
function parts(url){const m=String(url).match(/^([\w+.-]+:)?\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/)||[];const host=m[2]||'';return {href:String(url),protocol:m[1]||'',host,hostname:host.split(':')[0],port:host.split(':')[1]||'',pathname:m[3]||'/',search:m[4]||'',hash:m[5]||'',origin:(m[1]||'')+'//'+host}}
const location={assign(url){b('navigate',0,String(url),'false')},replace(url){b('navigate',0,String(url),'true')},reload(){b('navigate',0,b('url'),'true')},toString(){return this.href}};
for(const key of ['href','protocol','host','hostname','port','pathname','search','hash','origin'])Object.defineProperty(location,key,{get(){return parts(b('url'))[key]},set(value){if(key==='href')this.assign(value);else {const p=parts(b('url'));p[key]=String(value);this.assign(p.protocol+'//'+p.host+(key==='pathname'?value:p.pathname)+(key==='search'?value:p.search)+(key==='hash'?value:p.hash))}}});
class URL {constructor(value,base){Object.assign(this,parts(b('resolve-url',0,String(value),String(base||b('url')))));this.searchParams=new URLSearchParams(this.search)}toString(){return this.href}toJSON(){return this.href}}
class URLSearchParams {constructor(value=''){this._a=String(value).replace(/^\?/,'').split('&').filter(Boolean).map(x=>{const i=x.indexOf('=');return [decodeURIComponent((i<0?x:x.slice(0,i)).replace(/\+/g,' ')),decodeURIComponent((i<0?'':x.slice(i+1)).replace(/\+/g,' '))]})}get(k){const p=this._a.find(x=>x[0]===String(k));return p?p[1]:null}getAll(k){return this._a.filter(x=>x[0]===String(k)).map(x=>x[1])}has(k){return this._a.some(x=>x[0]===String(k))}append(k,v){this._a.push([String(k),String(v)])}set(k,v){this.delete(k);this.append(k,v)}delete(k){this._a=this._a.filter(x=>x[0]!==String(k))}toString(){return this._a.map(x=>x.map(v=>encodeURIComponent(v).replace(/%20/g,'+')).join('=')).join('&')}*[Symbol.iterator](){yield*this._a}}
let timerID=1,now=0,lastNativeSample=b('now'),timerClockMapped=false;const timers=new Map();
function timerNow(){const elapsed=b('now')-lastNativeSample;return now+(elapsed>0?elapsed:0)}
function timeout(fn,delay,repeat,args){if(timers.size>=256)return 0;const id=timerID++;timers.set(id,{fn,at:timerNow()+Math.max(0,Math.min(+delay||0,2147483647)),repeat,args});return id}
function pump(time){
 if(!timerClockMapped){const shift=time-timerNow();for(const timer of timers.values())timer.at+=shift;timerClockMapped=true}
 now=time;lastNativeSample=b('now');let count=0;const batchStart=b('now');
 for(const [id,t] of Array.from(timers)){if(t.at>now||count++>=32)continue;if(t.repeat)t.at=timerNow()+Math.max(4,t.repeat);else timers.delete(id);if(typeof t.fn==='function')t.fn(...t.args);else (0,eval)(String(t.fn));if(b('now')-batchStart>=8)break}
}
const observers=new Set();let observationPending=false;
class MutationObserver {
 constructor(callback){if(typeof callback!=='function')throw TypeError('MutationObserver callback');this.callback=callback;this.targets=[];this.records=[]}
 observe(target,options={}){if(!target||!(target instanceof Node))throw TypeError('MutationObserver target');if(options.attributeOldValue||options.attributeFilter)options.attributes=true;if(options.characterDataOldValue)options.characterData=true;if(!options.childList&&!options.attributes&&!options.characterData)throw TypeError('MutationObserver options');this.targets=this.targets.filter(x=>x.target!==target);this.targets.push({target,options});observers.add(this)}
 disconnect(){this.targets=[];this.records=[];observers.delete(this)} takeRecords(){const records=this.records;this.records=[];return records}
}
function notify(target,type,addedNodes=[],removedNodes=[],attributeName=null,oldValue=null){
 for(const observer of observers){for(const entry of observer.targets){const o=entry.options;if(target!==entry.target&&!(o.subtree&&entry.target.contains(target)))continue;if(!o[type]||(type==='attributes'&&o.attributeFilter&&!o.attributeFilter.includes(attributeName)))continue;observer.records.push({type,target,addedNodes,removedNodes,attributeName,attributeNamespace:null,oldValue:(type==='attributes'?o.attributeOldValue:o.characterDataOldValue)?oldValue:null,previousSibling:null,nextSibling:null});break}}
 if(!observationPending){observationPending=true;try{enqueueMicrotask(flushObservers)}catch(error){observationPending=false;throw error}}
}
function flushObservers(){observationPending=false;for(const observer of Array.from(observers)){const records=observer.takeRecords();if(records.length){try{observer.callback(records,observer)}catch(error){reportEventError(error,'MutationObserver callback')}}}}
function boundaryLength(node){return node.nodeType===3||node.nodeType===8?node.data.length:node.childNodes.length}
function treeRoot(node){while(node&&node.parentNode)node=node.parentNode;return node}
function comparePoints(a,ao,b,bo){
 if(a===b)return ao===bo?0:ao<bo?-1:1;
 const aa=[],bb=[];for(let n=a;n;n=n.parentNode)aa.unshift(n);for(let n=b;n;n=n.parentNode)bb.unshift(n);
 if(aa[0]!==bb[0])throw Error('WrongDocumentError');let i=0;while(i<aa.length&&i<bb.length&&aa[i]===bb[i])i++;
 if(i===aa.length)return ao<=a.childNodes.indexOf(bb[i])?-1:1;
 if(i===bb.length)return bo<=b.childNodes.indexOf(aa[i])?1:-1;
 const children=aa[i-1].childNodes;return children.indexOf(aa[i])<children.indexOf(bb[i])?-1:1;
}
function validBoundary(node,offset){if(!(node instanceof Node)||!Number.isInteger(offset)||offset<0||offset>boundaryLength(node))throw Error('IndexSizeError')}
class Range {
 constructor(){this.startContainer=document;this.startOffset=0;this.endContainer=document;this.endOffset=0}
 get collapsed(){return this.startContainer===this.endContainer&&this.startOffset===this.endOffset}
 get commonAncestorContainer(){let n=this.startContainer;while(n&&!n.contains(this.endContainer))n=n.parentNode;return n}
 setStart(node,offset){offset=Number(offset);validBoundary(node,offset);this.startContainer=node;this.startOffset=offset;if(treeRoot(node)!==treeRoot(this.endContainer)||comparePoints(node,offset,this.endContainer,this.endOffset)>0){this.endContainer=node;this.endOffset=offset}}
 setEnd(node,offset){offset=Number(offset);validBoundary(node,offset);this.endContainer=node;this.endOffset=offset;if(treeRoot(node)!==treeRoot(this.startContainer)||comparePoints(this.startContainer,this.startOffset,node,offset)>0){this.startContainer=node;this.startOffset=offset}}
 setStartBefore(node){if(!node.parentNode)throw Error('InvalidNodeTypeError');this.setStart(node.parentNode,node.parentNode.childNodes.indexOf(node))}
 setStartAfter(node){if(!node.parentNode)throw Error('InvalidNodeTypeError');this.setStart(node.parentNode,node.parentNode.childNodes.indexOf(node)+1)}
 setEndBefore(node){if(!node.parentNode)throw Error('InvalidNodeTypeError');this.setEnd(node.parentNode,node.parentNode.childNodes.indexOf(node))}
 setEndAfter(node){if(!node.parentNode)throw Error('InvalidNodeTypeError');this.setEnd(node.parentNode,node.parentNode.childNodes.indexOf(node)+1)}
 selectNode(node){this.setStartBefore(node);this.setEndAfter(node)} selectNodeContents(node){this.setStart(node,0);this.setEnd(node,boundaryLength(node))}
 collapse(toStart=false){if(toStart){this.endContainer=this.startContainer;this.endOffset=this.startOffset}else{this.startContainer=this.endContainer;this.startOffset=this.endOffset}}
 cloneRange(){const range=new Range();range.startContainer=this.startContainer;range.startOffset=this.startOffset;range.endContainer=this.endContainer;range.endOffset=this.endOffset;return range}
 comparePoint(node,offset){validBoundary(node,offset);if(comparePoints(node,offset,this.startContainer,this.startOffset)<0)return -1;if(comparePoints(node,offset,this.endContainer,this.endOffset)>0)return 1;return 0}
 isPointInRange(node,offset){return treeRoot(node)===treeRoot(this.startContainer)&&this.comparePoint(node,offset)===0}
 intersectsNode(node){if(treeRoot(node)!==treeRoot(this.startContainer))return false;const p=node.parentNode;if(!p)return true;const index=p.childNodes.indexOf(node);return comparePoints(p,index+1,this.startContainer,this.startOffset)>0&&comparePoints(p,index,this.endContainer,this.endOffset)<0}
 toString(){if(this.collapsed)return '';const pending=[this.commonAncestorContainer];let result='';while(pending.length){const node=pending.pop();if(node.nodeType===3){if(comparePoints(node,node.length,this.startContainer,this.startOffset)<=0||comparePoints(node,0,this.endContainer,this.endOffset)>=0)continue;result+=node.data.slice(node===this.startContainer?this.startOffset:0,node===this.endContainer?this.endOffset:node.length)}else{const children=node.childNodes;for(let i=children.length-1;i>=0;i--)pending.push(children[i])}}return result}
 detach(){}
}
Object.assign(Range,{START_TO_START:0,START_TO_END:1,END_TO_END:2,END_TO_START:3});Object.assign(Range.prototype,Range);
class Selection {
 constructor(){this._range=null;this._backward=false}
 get rangeCount(){return this._range?1:0} get isCollapsed(){return !this._range||this._range.collapsed} get type(){return !this._range?'None':this.isCollapsed?'Caret':'Range'}
 get anchorNode(){return this._range?(this._backward?this._range.endContainer:this._range.startContainer):null} get focusNode(){return this._range?(this._backward?this._range.startContainer:this._range.endContainer):null}
 get anchorOffset(){return this._range?(this._backward?this._range.endOffset:this._range.startOffset):0} get focusOffset(){return this._range?(this._backward?this._range.startOffset:this._range.endOffset):0}
 getRangeAt(index){if(index!==0||!this._range)throw Error('IndexSizeError');return this._range}
 addRange(range){if(!(range instanceof Range))throw TypeError('Range required');if(!this._range){this._range=range;this._backward=false}}
 removeRange(range){if(this._range!==range)throw Error('NotFoundError');this.removeAllRanges()} removeAllRanges(){this._range=null;this._backward=false} empty(){this.removeAllRanges()}
 collapse(node,offset=0){if(node===null){this.removeAllRanges();return}const range=new Range();range.setStart(node,offset);range.collapse(true);this._range=range;this._backward=false}
 setPosition(node,offset=0){this.collapse(node,offset)}
 setBaseAndExtent(anchor,anchorOffset,focus,focusOffset){validBoundary(anchor,anchorOffset);validBoundary(focus,focusOffset);const range=new Range();this._backward=comparePoints(anchor,anchorOffset,focus,focusOffset)>0;range.setStart(this._backward?focus:anchor,this._backward?focusOffset:anchorOffset);range.setEnd(this._backward?anchor:focus,this._backward?anchorOffset:focusOffset);this._range=range}
 extend(node,offset=0){if(!this._range)throw Error('InvalidStateError');this.setBaseAndExtent(this.anchorNode,this.anchorOffset,node,offset)}
 collapseToStart(){if(!this._range)throw Error('InvalidStateError');this._range.collapse(true);this._backward=false} collapseToEnd(){if(!this._range)throw Error('InvalidStateError');this._range.collapse(false);this._backward=false}
 selectAllChildren(node){const range=new Range();range.selectNodeContents(node);this._range=range;this._backward=false}
 containsNode(node,partial=false){if(!this._range)return false;if(partial)return this._range.intersectsNode(node);if(!node.parentNode)return false;const index=node.parentNode.childNodes.indexOf(node);return this._range.isPointInRange(node.parentNode,index)&&this._range.isPointInRange(node.parentNode,index+1)}
 toString(){return this._range?this._range.toString():''}
}
const selection=new Selection();
class XMLHttpRequest {
 constructor(){this.readyState=0;this.status=0;this.statusText='';this.responseText='';this.response=null;this.responseType='';this.responseURL='';this.timeout=0;this.withCredentials=false;this.upload={addEventListener(){},removeEventListener(){}};this._version=0;this._headers={}}
 addEventListener(...args){return add.apply(this,args)} removeEventListener(...args){return remove.apply(this,args)} dispatchEvent(event){return dispatch.call(this,event)}
 open(method,url,async=true){this._method=String(method).toUpperCase();this._url=b('resolve',0,String(url));this._async=async!==false;this._headers={};this._version++;this.readyState=1;this.dispatchEvent(new Event('readystatechange',{bubbles:false}))}
 setRequestHeader(name,value){if(this.readyState!==1)throw Error('InvalidStateError');this._headers[String(name).toLowerCase()]=String(value)}
 overrideMimeType(type){this._mime=String(type)}
 getAllResponseHeaders(){return this.readyState>=2&&this._contentType?'content-type: '+this._contentType+'\r\n':''}
 getResponseHeader(name){return String(name).toLowerCase()==='content-type'?this._contentType||null:null}
 abort(){this._version++;this.readyState=0;this.status=0;this.dispatchEvent(new Event('abort',{bubbles:false}));this.dispatchEvent(new Event('loadend',{bubbles:false}))}
 send(){if(this.readyState!==1)throw Error('InvalidStateError');const version=this._version;const run=()=>{if(version!==this._version)return;let response;const headers=Object.keys(this._headers).filter(k=>!['cookie','host','connection','content-length','origin','referer','user-agent'].includes(k));if(headers.length)response={status:0,error:'Custom request headers are not supported'};else if(!['','text','json'].includes(this.responseType))response={status:0,error:'Unsupported responseType'};else response=b('request',0,this._url,this._method);this.status=response.status||0;this.responseURL=response.url||this._url;this._contentType=response.contentType||'';this.readyState=2;this.dispatchEvent(new Event('readystatechange',{bubbles:false}));this.responseText=response.body||'';this.readyState=4;this.response=this.responseText;if(this.responseType==='json'){try{this.response=JSON.parse(this.responseText)}catch(e){this.response=null}}this.dispatchEvent(new Event('readystatechange',{bubbles:false}));this.dispatchEvent(new Event(response.error?'error':'load',{bubbles:false}));this.dispatchEvent(new Event('loadend',{bubbles:false}))};if(this._async)setTimeout(run,0);else run()}
}
Object.assign(XMLHttpRequest,{UNSENT:0,OPENED:1,HEADERS_RECEIVED:2,LOADING:3,DONE:4});Object.assign(XMLHttpRequest.prototype,XMLHttpRequest);
class Storage {
 constructor(){Object.defineProperty(this,'_values',{value:new Map()})}
 get length(){return this._values.size} key(index){return Array.from(this._values.keys())[index]??null}
 getItem(key){key=String(key);return this._values.has(key)?this._values.get(key):null}
 setItem(key,value){key=String(key);value=String(value);let size=key.length+value.length;for(const [k,v] of this._values)if(k!==key)size+=k.length+v.length;if(size>2*1024*1024)throw Error('QuotaExceededError');this._values.set(key,value)}
 removeItem(key){this._values.delete(String(key))} clear(){this._values.clear()}
}
const storage=()=>new Proxy(new Storage(),{get(t,k,r){return Reflect.has(t,k)?Reflect.get(t,k,r):t.getItem(k)},set(t,k,v){t.setItem(k,v);return true},deleteProperty(t,k){t.removeItem(k);return true},ownKeys(t){return ['_values',...Array.from(t._values.keys()).filter(k=>k!=='_values')]},getOwnPropertyDescriptor(t,k){if(k==='_values')return Reflect.getOwnPropertyDescriptor(t,k);return t._values.has(k)?{value:t.getItem(k),writable:true,enumerable:true,configurable:true}:undefined}});
const base64chars='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function atob(input){let source=String(input).replace(/[\t\n\f\r ]/g,'');if(source.length%4===1||/[^A-Za-z0-9+/=]/.test(source)||(/=/.test(source)&&(source.length%4!==0||!/^[A-Za-z0-9+/]*={1,2}$/.test(source))))throw Error('InvalidCharacterError');source=source.replace(/=+$/,'');let buffer=0,bits=0,result='';for(const character of source){buffer=(buffer<<6)|base64chars.indexOf(character);bits+=6;if(bits>=8){bits-=8;result+=String.fromCharCode((buffer>>bits)&255)}}return result}
function btoa(input){const source=String(input);let result='';for(let i=0;i<source.length;i+=3){const a=source.charCodeAt(i),b=source.charCodeAt(i+1),c=source.charCodeAt(i+2);if(a>255||b>255||c>255)throw Error('InvalidCharacterError');const value=(a<<16)|((b||0)<<8)|(c||0);result+=base64chars[(value>>>18)&63]+base64chars[(value>>>12)&63]+(i+1<source.length?base64chars[(value>>>6)&63]:'=')+(i+2<source.length?base64chars[value&63]:'=')}return result}
// Native history owns entry order. Serialized page state is opaque to the host;
// graph references preserve cycles without retaining page objects in native code.
function stateError(message){throw new DOMException(message,'DataCloneError')}
function encodeHistoryState(value){
 const records=[],seen=new Map();let propertyCount=0,payloadUnits=0;
 function charge(units){payloadUnits+=units;if(payloadUnits>262144)stateError('History state payload limit exceeded')}
 function encode(value,depth){
  if(depth>128)stateError('History state depth limit exceeded');
  if(value===null)return ['null'];
  const type=typeof value;
  if(type==='undefined')return ['undefined'];if(type==='string'){charge(value.length);return [type,value]}if(type==='boolean')return [type,value];
  if(type==='number')return ['number',Object.is(value,-0)?'-0':String(value)];if(type==='bigint'){const text=String(value);charge(text.length);return ['bigint',text]}
  if(type!=='object'||value instanceof Node||value instanceof Event||value instanceof History||value instanceof Storage||value instanceof Range||value instanceof Selection||value instanceof XMLHttpRequest)stateError('History state contains an uncloneable value');
  if(seen.has(value))return ['reference',seen.get(value)];
  if(records.length>=4096)stateError('History state object limit exceeded');
  const index=records.length;seen.set(value,index);records.push(null);let record;
  const tag=Object.prototype.toString.call(value);
  if(Array.isArray(value)||tag==='[object Object]'){
   const entries=[];for(const key of Object.keys(value)){charge(key.length);if(++propertyCount>16384)stateError('History state property limit exceeded');entries.push([key,encode(value[key],depth+1)])}
   record=Array.isArray(value)?['array',value.length,entries]:['object',entries];
  }else if(value instanceof Date)record=['date',String(value.getTime())];
  else if(value instanceof RegExp){charge(value.source.length);record=['regexp',value.source,value.flags]}
  else if(value instanceof Map){const entries=[];for(const [key,item]of value){if(++propertyCount>16384)stateError('History state property limit exceeded');entries.push([encode(key,depth+1),encode(item,depth+1)])}record=['map',entries]}
  else if(value instanceof Set){const entries=[];for(const item of value){if(++propertyCount>16384)stateError('History state property limit exceeded');entries.push(encode(item,depth+1))}record=['set',entries]}
  else if(value instanceof ArrayBuffer){charge(value.byteLength);record=['buffer',Array.from(new Uint8Array(value))]}
  else if(ArrayBuffer.isView(value)){const name=tag.slice(8,-1);if(!['DataView','Int8Array','Uint8Array','Uint8ClampedArray','Int16Array','Uint16Array','Int32Array','Uint32Array','Float32Array','Float64Array','BigInt64Array','BigUint64Array'].includes(name))stateError('Unsupported history buffer view');record=['view',name,encode(value.buffer,depth+1),value.byteOffset,name==='DataView'?value.byteLength:value.length]}
  else stateError('Unsupported history state object');
  records[index]=record;return ['reference',index];
 }
 const root=encode(value,0),json=JSON.stringify(['XJ380HistoryState',1,root,records]);let bytes=0;
 for(const character of json){const point=character.codePointAt(0);bytes+=point<128?1:point<2048?2:point<65536?3:4;if(bytes>262144)stateError('History state byte limit exceeded')}
 return json;
}
function decodeHistoryState(json){
 const data=JSON.parse(json||'null');
 if(!Array.isArray(data)||data[0]!=='XJ380HistoryState'||data[1]!==1)return data;
 const records=data[3];if(!Array.isArray(records)||records.length>4096)stateError('Invalid history state');
 const objects=new Array(records.length);
 function decode(value){switch(value[0]){case'null':return null;case'undefined':return undefined;case'string':case'boolean':return value[1];case'number':return Number(value[1]);case'bigint':return BigInt(value[1]);case'reference':if(!Number.isInteger(value[1])||value[1]<0||value[1]>=objects.length)stateError('Invalid history reference');return objects[value[1]];default:stateError('Invalid history value')}}
 for(let i=0;i<records.length;i++){const record=records[i];switch(record[0]){case'array':objects[i]=new Array(record[1]);break;case'object':objects[i]={};break;case'date':objects[i]=new Date(Number(record[1]));break;case'regexp':objects[i]=new RegExp(record[1],record[2]);break;case'map':objects[i]=new Map();break;case'set':objects[i]=new Set();break;case'buffer':objects[i]=Uint8Array.from(record[1]).buffer;break;case'view':break;default:stateError('Invalid history record')}}
 for(let i=0;i<records.length;i++){const record=records[i];if(record[0]==='view'){const constructor=globalThis[record[1]],buffer=decode(record[2]);if(typeof constructor!=='function'||!(buffer instanceof ArrayBuffer))stateError('Invalid history view');objects[i]=new constructor(buffer,record[3],record[4])}}
 for(let i=0;i<records.length;i++){const record=records[i],object=objects[i],entries=record[0]==='array'?record[2]:record[1];if(record[0]==='object'||record[0]==='array')for(const [key,value]of entries)Object.defineProperty(object,key,{value:decode(value),writable:true,enumerable:true,configurable:true});else if(record[0]==='map')for(const [key,value]of entries)object.set(decode(key),decode(value));else if(record[0]==='set')for(const value of entries)object.add(decode(value))}
 return decode(data[2]);
}
let historyStateJSON=null,historyStateValue=null,historyDocumentURL=b('url');
function historyState(json){json=String(json||'null');if(json!==historyStateJSON){historyStateValue=decodeHistoryState(json);historyStateJSON=json}return historyStateValue}
function historyUpdate(value,url,replace){
 const resolved=url===undefined?b('url'):b('resolve',0,String(url));
 if(parts(resolved).origin!==parts(b('url')).origin)throw new DOMException('History URL must have the same origin','SecurityError');
 const encoded=encodeHistoryState(value);
 if(!b('history-update',replace?1:0,resolved,encoded))throw new DOMException('History update rejected','SecurityError');
 historyStateJSON=encoded;historyStateValue=decodeHistoryState(encoded);historyDocumentURL=resolved;
}
class History {
 get length(){return b('history-length')}
 get state(){return historyState(b('history-state'))}
 pushState(value,unused,url){historyUpdate(value,url,false)} replaceState(value,unused,url){historyUpdate(value,url,true)}
 go(delta=0){delta=Number(delta);if(!Number.isFinite(delta))delta=0;delta=Math.trunc(delta);if(delta<-2147483648||delta>2147483647)delta=0;b('history-go',0,String(delta))}
 back(){this.go(-1)} forward(){this.go(1)}
}
class PopStateEvent extends Event {constructor(type,options={}){super(type,options);this.state=options.state===undefined?null:options.state}}
class HashChangeEvent extends Event {constructor(type,options={}){super(type,options);this.oldURL=String(options.oldURL||'');this.newURL=String(options.newURL||'')}}
const history=new History();
function historyChanged(url,json){const previous=historyDocumentURL;historyDocumentURL=String(url);historyStateJSON=null;const state=historyState(json);dispatch.call(globalThis,new PopStateEvent('popstate',{state}),true);if(parts(previous).hash!==parts(url).hash)dispatch.call(globalThis,new HashChangeEvent('hashchange',{oldURL:previous,newURL:String(url)}),true)}
function viewport(){return b('viewport')}
function scrollToPosition(x,y,relative=false){const view=viewport();if(x&&typeof x==='object'){const options=x;x=options.left===undefined?view.scrollX:Number(options.left);y=options.top===undefined?view.scrollY:Number(options.top)}else{x=Number(x);y=Number(y)}if(!Number.isFinite(x))x=0;if(!Number.isFinite(y))y=0;if(relative){x+=view.scrollX;y+=view.scrollY}b('scroll',0,String(x),String(y))}
Object.assign(globalThis,{window:globalThis,self:globalThis,atob,btoa,top:globalThis,parent:globalThis,frames:globalThis,document,navigator:{mimeTypes:[],plugins:[],vendor:'',product:'Gecko',appCodeName:'Mozilla',hardwareConcurrency:1,javaEnabled:()=>false,userAgent:'XJ380Browser/0.2',appName:'Netscape',appVersion:'5.0',platform:'XJ380',language:'zh-CN',languages:['zh-CN','zh'],cookieEnabled:true,onLine:true,maxTouchPoints:0},screen:{width:1024,height:768,availWidth:1024,availHeight:740,colorDepth:24,pixelDepth:24},innerWidth:1024,innerHeight:700,outerWidth:1024,outerHeight:768,devicePixelRatio:1,pageXOffset:0,pageYOffset:0,scrollX:0,scrollY:0,History,PopStateEvent,HashChangeEvent,history,Node,Element,HTMLElement,SVGElement,MathMLElement,DocumentFragment,HTMLTemplateElement,HTMLLinkElement,DOMException,HTMLInputElement:Element,HTMLFormElement:Element,HTMLAnchorElement,HTMLAreaElement:HTMLAnchorElement,Document,MutationObserver,XMLHttpRequest,Storage,Range,Selection,getSelection:()=>selection,Event,MouseEvent:Event,KeyboardEvent:Event,InputEvent:Event,CustomEvent:class extends Event{constructor(t,o={}){super(t,o);this.detail=o.detail}},URL,URLSearchParams,localStorage:storage(),sessionStorage:storage(),console:{log(...a){b('log',0,a.map(x=>x&&x.stack?String(x)+'\n'+x.stack:String(x)).join(' '))},warn(...a){b('log',0,a.map(x=>x&&x.stack?String(x)+'\n'+x.stack:String(x)).join(' '))},error(...a){b('log',0,a.map(x=>x&&x.stack?String(x)+'\n'+x.stack:String(x)).join(' '))},info(...a){b('log',0,a.map(x=>x&&x.stack?String(x)+'\n'+x.stack:String(x)).join(' '))},debug(){},time(){},timeEnd(){}},performance:{now:()=>b('now'),timeOrigin:Date.now(),timing:{navigationStart:Date.now()}},setTimeout:(fn,delay,...args)=>timeout(fn,delay,0,args),setInterval:(fn,delay,...args)=>timeout(fn,delay,Math.max(4,+delay||0),args),clearTimeout:id=>timers.delete(id),clearInterval:id=>timers.delete(id),requestAnimationFrame:fn=>timeout(()=>fn(b('now')),16,0,[]),cancelAnimationFrame:id=>timers.delete(id),requestIdleCallback:fn=>timeout(()=>{const end=b('now')+8;fn({didTimeout:false,timeRemaining:()=>{const remaining=end-b('now');return remaining>0?remaining:0}})},1,0,[]),cancelIdleCallback:id=>timers.delete(id),queueMicrotask:enqueueMicrotask,addEventListener:(...args)=>add.apply(globalThis,args),removeEventListener:(...args)=>remove.apply(globalThis,args),dispatchEvent:event=>dispatch.call(globalThis,event),alert:message=>b('log',0,String(message)),confirm:()=>false,prompt:()=>null,open:url=>{b('navigate',0,String(url),'false');return globalThis},close(){},scrollTo:(x,y)=>scrollToPosition(x,y),scroll:(x,y)=>scrollToPosition(x,y),scrollBy:(x,y)=>scrollToPosition(x,y,true),matchMedia:query=>({matches:false,media:String(query),addListener(){},removeListener(){},addEventListener(){},removeEventListener(){}}),Image:function(w,h){const e=document.createElement('img');if(w)e.width=w;if(h)e.height=h;return e}});
for(const [name,field]of Object.entries({innerWidth:'width',innerHeight:'height',outerWidth:'outerWidth',outerHeight:'outerHeight',scrollX:'scrollX',pageXOffset:'scrollX',scrollY:'scrollY',pageYOffset:'scrollY'}))Object.defineProperty(globalThis,name,{configurable:true,enumerable:true,get:()=>viewport()[field]});
Object.defineProperty(globalThis,'location',{get:()=>location,set:v=>location.assign(v)});
Object.assign(globalThis,{getComputedStyle:e=>new Proxy(e.style,{get(t,k){const v=t[k];if(v)return v;return b('computed',e._id,cssName(k))}}),__historyChanged:historyChanged,__viewChanged:type=>{if(type==='resize'||type==='scroll')dispatch.call(globalThis,new Event(type),true)},__dispatch:(id,type,key,code)=>{const target=id?node(id):['keydown','keyup','keypress'].includes(type)?document.activeElement||document:document;if(type==='focus')document.activeElement=target;if(type==='blur'&&document.activeElement===target)document.activeElement=document.body;const mouse=['click','dblclick','mousedown','mouseup'].includes(type),bubbles=!['load','error','focus','blur','mouseenter','mouseleave'].includes(type),cancelable=mouse||['keydown','keyup','keypress','submit','contextmenu','beforeinput'].includes(type);const event=new Event(type,{bubbles,cancelable,key:key||'',code:key||'',keyCode:code||0,which:mouse?1:code||0});if(mouse)Object.assign(event,{button:0,buttons:type==='mousedown'?1:0,detail:type==='dblclick'?2:1});return !dispatch.call(target,event,true)},__lifecycle:type=>{if(type==='DOMContentLoaded')document.readyState='interactive';if(type==='load')document.readyState='complete';dispatch.call(type==='load'?globalThis:document,new Event(type,{bubbles:type==='DOMContentLoaded'}),true)},__currentScript:id=>{document.currentScript=node(id)},__node:id=>node(id),__pump:time=>{pump(time)}});
initializeTemplates(document);
})();
)JS";

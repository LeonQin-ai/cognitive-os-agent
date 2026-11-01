const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../apps/web/index.html'),'utf8');
class Element {
  constructor(tag,text=''){this.tag=tag;this.children=[];this.style={};this.textContent=text;}
  append(...items){this.children.push(...items);}
}
const context={document:{createElement:tag=>new Element(tag),createTextNode:text=>new Element('#text',text)},
  chatImageSource:value=>/^https:\/\//.test(value)?value:'',
  chatImageButton:(src,alt)=>new Element('image',src+' '+alt)};
vm.createContext(context);
const source=html.slice(html.indexOf('function chatRenderInline('),html.indexOf('function chatRenderImages('));
vm.runInContext(source,context);
const root=new Element('root');
context.chatRenderText(root,'# Result\n**Verified** `sample`\n![proof](https://example.test/proof.png)\n```md\n![literal](https://example.test/literal.png)\n```\n<script>alert(1)</script>\n![bad](javascript:alert)');
const nodes=[];function visit(node){nodes.push(node);node.children.forEach(visit);}visit(root);
assert.equal(nodes.filter(n=>n.tag==='h1').length,1);
assert.equal(nodes.filter(n=>n.tag==='strong'&&n.textContent==='Verified').length,1);
assert.equal(nodes.filter(n=>n.tag==='image').length,1);
assert(nodes.some(n=>n.tag==='#text'&&n.textContent.includes('![literal]')));
assert(nodes.some(n=>n.tag==='#text'&&n.textContent.includes('<script>')));
assert(!nodes.some(n=>n.tag==='script'));
console.log('PASS Markdown headings, emphasis, image preview, literal code and safe text');

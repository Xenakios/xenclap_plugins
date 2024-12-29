plugin_id = "com.xenakios.gritnoise"
plugin_class_name = "GritNoise"
params = [
    {
        "name": "Grit density",
        "minval": 0.0,
        "maxval": 1.0,
        "defaultval": 0.5,
        "units": "%",
    },
    {
        "name": "Pulse decay",
        "minval": 0.0,
        "maxval": 1.0,
        "defaultval": 0.0,
        "units": "%",
    },
]

usechocwebview = False

outfilename = "generated.cpp"

with open(r"C:\develop\xen_clap_plugins\python\clapgen\template.cpp", "r") as infile:
    codetxt = infile.read()
    transformed = codetxt.replace("$PLUGINCLASSNAME$", plugin_class_name)
    transformed = transformed.replace("$PLUGIN_ID$", plugin_id)

    if usechocwebview:
        transformed = transformed.replace(
            "// $INCLUDES_SECTION$", '#include "gui/choc_WebView.h"'
        )
    else:
        transformed = transformed.replace("// $INCLUDES_SECTION$", "")
    clapid = 10
    parsection = ""
    for par in params:
        parsection += f"paramDescriptions.push_back(make_simple_parameter_desc(\"{par["name"]}\", {clapid}, {par["minval"]}, {par["maxval"]}, {par["defaultval"]}, \"{par["units"]}\"));\n"
        clapid += 1000
    transformed = transformed.replace("// $INITPARAMETERMETADATA$", parsection)

    with open(
        f"C:\\develop\\xen_clap_plugins\\python\\clapgen\\{outfilename}", "w"
    ) as outfile:
        outfile.write(transformed)

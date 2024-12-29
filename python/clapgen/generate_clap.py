params = [
    {
        "name": "Volume",
        "minval": -60.0,
        "maxval": 0.0,
        "defaultval": -3.0,
        "units": "dB",
    },
    {"name": "Pan", "minval": -1.0, "maxval": 1.0, "defaultval": 0.0, "units": "%"},
]

usechocwebview = False

with open(r"C:\develop\xen_clap_plugins\python\clapgen\template.cpp", "r") as infile:
    codetxt = infile.read()
    transformed = codetxt.replace("$PLUGINCLASSNAME$", "GritNoise")
    if usechocwebview:
        transformed = codetxt.replace("// $INCLUDES_SECTION$", "#include \"gui/choc_WebView.h\"")
    else:
        transformed = codetxt.replace("// $INCLUDES_SECTION$", "")
    clapid = 10
    parsection = ""
    for par in params:
        parsection += f"paramDescriptions.push_back(make_simple_parameter_desc(\"{par["name"]}\", {clapid}, {par["minval"]}, {par["maxval"]}, {par["defaultval"]}, \"{par["units"]}\"));\n"
        clapid += 100
    transformed = transformed.replace("// $INITPARAMETERMETADATA$", parsection)

    with open(
        r"C:\develop\xen_clap_plugins\python\clapgen\generated.cpp", "w"
    ) as outfile:
        outfile.write(transformed)

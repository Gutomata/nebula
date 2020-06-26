/*
 * Copyright 2017-present Shawn Cao
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * This is entry point of the web server served by node JS
 */
const http = require('http');
const url = require('url');

// service call module
const NebulaClient = require('./dist/node/main');
const serviceAddr = process.env.NS_ADDR || "dev-shawncao:9190";
const comments_table = "pin.comments";
const pins_table = "pin.pins";
const signatures_table = "pin.signatures";
const advertisers_table = "advertisers";
const advertisers_spend_table = "advertisers.spend";
const promoted_pins = "pin.promote.rich";
const client = NebulaClient.qc(serviceAddr);
const grpc = NebulaClient.grpc;
const timeCol = "_time_";
const seconds = (ds) => (+ds == ds) ?
    Math.round(new Date(+ds).getTime() / 1000) :
    Math.round(new Date(ds).getTime() / 1000);
const error = (msg) => {
    return JSON.stringify({
        "error": msg,
        "duration": 0
    });
};

// TODO(cao): these should go more flexible way to figure user info after auth
// Right now, it is only one example way.
const AuthHeader = "authorization";
const UserHeader = "x-forwarded-user";
const GroupHeader = "x-forwarded-groups";

/**
 * Query API
 * start: 2019-04-01
 * end: 2019-06-20
 * fc: user_id
 * op: 0=, 1!=, 2>,3<, 4like
 * fv: 23455554444
 * cols: ["user_id", "pin_id"]
 * limit: 1000
 * res: http response
 */
class Handler {
    constructor() {
        this.response = null;
        this.metadata = {};
        this.onError = (err) => {
            this.response.write(error(`${err}`));
            this.response.end();
        };
        this.onNull = () => {
            this.response.write(error("Failed to get reply"));
            this.response.end();
        };
        this.onSuccess = (data) => {
            this.response.write(data);
            this.response.end();
        };
        this.endWithMessage = (message) => {
            this.response.write(error(message));
            this.response.end();
        }
    }
};

const query = (table, start, end, fc, op, fv, cols, metrics, order, limit, handler) => {
    const req = new NebulaClient.QueryRequest();
    req.setTable(table);
    req.setStart(seconds(start));
    req.setEnd(seconds(end));

    // single filter
    const p2 = new NebulaClient.Predicate();
    p2.setColumn(fc);
    p2.setOp(op);
    p2.setValueList(Array.isArray(fv) ? fv : [fv]);
    const filter = new NebulaClient.PredicateAnd();
    filter.setExpressionList([p2]);
    req.setFiltera(filter);

    // set dimension
    req.setDimensionList(cols);

    if (metrics.length > 0) {
        req.setMetricList(metrics);
    }

    if (order) {
        req.setOrder(order);
    }

    // set limit
    req.setTop(limit);

    // do the query
    client.query(req, handler.metadata, (err, reply) => {
        if (err !== null) {
            return handler.onError(err);
        }

        if (reply == null) {
            return handler.onNull();
        }
        return handler.onSuccess(NebulaClient.bytes2utf8(reply.getData()));
    });
};

/**
 * Count pins per domain given time range with filter 
 */
const getPinsPerDomainWithKey = (q, handler) => {
    console.log(`querying pins for details like ${q.key}`);
    if (q.key) {
        // build metrics
        const m = new NebulaClient.Metric();
        m.setColumn("id");
        m.setMethod(1);

        // set order on metric only means we don't order on samples for now
        const o = new NebulaClient.Order();
        o.setColumn("id");
        o.setType(1);
        // search user Id
        query(pins_table, q.start, q.end, "details", "4", `%${q.key}%`,
            ["_time_", "link_domain"],
            [m], o,
            q.limit || 10000, handler);
        return;
    }

    handler.endWithMessage("Missing key");
};

/**
 * get all comments for given user id
 */
const getCommentsByUserId = (q, handler) => {
    console.log(`querying comments for user_id=${q.user_id}`);
    if (q.user_id) {
        // search user Id
        query(comments_table, q.start, q.end, "user_id", "0", q.user_id,
            ["_time_", "pin_signature", "comments"],
            [], null,
            q.limit || 100, handler);
        return;
    }

    handler.endWithMessage("Missing user_id.");
};

/**
 * get all comments for given pin id
 */
const getCommentsByPinSignature = (q, handler) => {
    console.log(`querying comments for pin_signature=${q.pin_signature}`);
    if (q.pin_signature) {
        // search pin Id
        query(comments_table, q.start, q.end, "pin_signature", "0", q.pin_signature,
            ["_time_", "user_id", "comments"],
            [], null,
            q.limit || 100, handler);
        return;
    }

    handler.endWithMessage("Missing pin_signature.");
};

/**
 * get all pins for given pin signature
 */
const getPinsBySignature = (q, handler) => {
    console.log(`querying comments for pin_signature=${q.pin_signature}`);
    if (q.pin_signature) {
        // search pin Id
        query(signatures_table, q.start, q.end, "pin_signature", "0", q.pin_signature,
            ["pin_id"],
            [], null,
            q.limit || 100, handler);
        return;
    }

    handler.endWithMessage("Missing pin_signature.");
};

/**
 * get signature for given pin
 */
const getSignatureByPin = (q, handler) => {
    console.log(`querying comments for pin_id=${q.pin_id}`);
    if (q.pin_id) {
        // search pin Id
        query(signatures_table, q.start, q.end, "pin_id", "0", q.pin_id,
            ["pin_signature"],
            [], null,
            q.limit || 100, handler);
        return;
    }

    handler.endWithMessage("Missing pin_id.");
};

/**
 * get all comments for given pin id
 */
const getCommentsByPattern = (q, handler) => {
    console.log(`querying comments for comments like ${q.pattern}`);
    if (q.pattern) {
        // search comments like
        query(comments_table, q.start, q.end, "comments", "4", q.pattern,
            ["_time_", "user_id", "pin_signature", "comments"],
            [], null,
            q.limit || 100, handler);
        return;
    }

    handler.endWithMessage("Missing pattern.");
};

/**
 * get all advertisers matching given key
 */
const getAdvertisersMatchingKey = (q, handler) => {
    console.log(`querying advertisers for name like ${q.key}`);
    if (q.key && q.key.length > 1) {
        // search advertisers by keyword
        query(advertisers_table, q.start, q.end, "name", "4", `%${q.key}%`,
            ["id", "name", "active", "country"],
            [], null,
            q.limit || 1000, handler);
        return;
    }

    handler.endWithMessage("Missing key.");
};

/**
 * get spend of each advertiser
 */
const getAdvertisersSpend = (q, handler) => {
    console.log(`querying advertisers spend`);
    if (q.ids && q.ids.length > 0) {
        // expect an JSON array
        const idArray = JSON.parse(q.ids);
        if (idArray && idArray.length > 0) {
            // build metrics sum(spend)
            const m = new NebulaClient.Metric();
            m.setColumn("spend");
            m.setMethod(0);

            // order by sum(spend) desc
            const o = new NebulaClient.Order();
            o.setColumn("spend");
            o.setType(1);

            // search advertisers id list = > "id in [id1, id2, ...]"
            query(advertisers_spend_table, q.start, q.end, "id", "0", `${idArray}`,
                ["id"],
                [m], o,
                q.limit || 1000, handler);
            return;
        }
    }

    handler.endWithMessage("Missing id list.");
};

/**
 * get advertisers with spend by given key
 */
const getAdvertisersWithSpendByKey = (q, handler) => {
    console.log(`querying advertisers for name like ${q.pattern}`);
    if (q.pattern && q.pattern.length > 1) {
        const MAX_ITEMS = 1000;
        // search advertisers by keyword
        // set handler to do the second query
        const tempHandler = new Handler();

        // share the same resposne stream
        tempHandler.response = handler.response;
        tempHandler.onSuccess = (dataStr) => {
            const data = JSON.parse(dataStr);
            const idArray = data.map(e => e['id']);
            // query all spend for this id list
            // build metrics sum(spend)
            const m = new NebulaClient.Metric();
            m.setColumn("spend");
            m.setMethod(0);

            // order by sum(spend) desc
            const o = new NebulaClient.Order();
            o.setColumn("spend");
            o.setType(1);

            // search advertisers id list = > "id in [id1, id2, ...]"
            const spendHandler = new Handler();
            // share the same response handler
            spendHandler.response = handler.response;
            spendHandler.onSuccess = (spendStr) => {
                const spend = JSON.parse(spendStr);
                // set spend data to each data item if not 
                // convert this spend array to key-value dictionary
                const spendMap = {};
                for (var i = 0; i < spend.length; ++i) {
                    var item = spend[i];
                    spendMap[item.id] = item['spend.sum'];
                }

                // for all data items, 
                for (var i = 0; i < data.length; ++i) {
                    var obj = data[i];
                    if (obj.id in spendMap) {
                        obj['spend'] = spendMap[obj.id];
                    } else {
                        // no spend, assign 0 to it.
                        obj['spend'] = 0;
                    }
                }

                // write data out like normal
                handler.onSuccess(JSON.stringify(data));
            };

            const sstart = q.sstart || q.start;
            const send = q.send || q.end;
            query(advertisers_spend_table, sstart, send, "id", "0", idArray,
                ["id"],
                [m], o,
                q.limit || MAX_ITEMS, spendHandler);
        };

        query(advertisers_table, q.start, q.end, "name", "5", q.pattern,
            ["id", "name", "active", "country", "owner_id", "billing_profile_id"],
            [], null,
            q.limit || MAX_ITEMS, tempHandler);
        return;
    }

    handler.endWithMessage("Missing pattern.");
};

/**
 * Search all promoted pins with specified keyword
 */
const getPinsByKeyword = (q, handler) => {
    console.log(`querying promoted pins for details/annotations like ${q.key}`);
    if (q.key) {
        const req = new NebulaClient.QueryRequest();
        req.setTable(promoted_pins);
        req.setStart(seconds(q.start));
        req.setEnd(seconds(q.end));

        const conditions = [];
        // apply the filter on different columns using OR
        let columnsToSearch = ["title", "details", "annotations"];
        if (q.c2s) {
            // if user specify what columns to search on, use it
            // accept json array
            columnsToSearch = JSON.parse(q.c2s);
        }

        for (let i = 0; i < columnsToSearch.length; ++i) {
            let p = new NebulaClient.Predicate();
            p.setColumn(columnsToSearch[i]);
            p.setOp("5");
            p.setValueList([`%${q.key}%`]);
            conditions.push(p);
        }

        const filter = new NebulaClient.PredicateOr();
        filter.setExpressionList(conditions);
        req.setFiltero(filter);

        // set dimension
        req.setDimensionList([
            "id", "image_signature", "title", "advertiser", "link", "private", "user_id", "board_id",
            "likes", "engages", "impressions", "clicks", "repins", "_time_"
        ]);

        // set limit to 1K results
        req.setTop(1000);

        // do the query
        client.query(req, handler.metadata, (err, reply) => {
            if (err !== null) {
                return handler.onError(err);
            }

            if (reply == null) {
                return handler.onNull();
            }
            return handler.onSuccess(NebulaClient.bytes2utf8(reply.getData()));
        });

        return;
    }

    handler.endWithMessage("Missing key");
};

/**
 * List all apis
 */
const listApi = (q) => {
    return JSON.stringify(Object.keys(api_handlers));
};

/**
 * Current user info through OAuth
 * q: query object
 * h: request headers
 */
const userInfo = (q, h) => {
    const info = {
        auth: 0
    };

    if (UserHeader in h) {
        info.auth = 1;
        info.authorization = h[AuthHeader] || "NONE";
        info.user = h[UserHeader];
        info.groups = h[GroupHeader];
    }

    return info;
};
/**
 * Convert user info into nebula headers as metadata for grpc service
 */
const toMetadata = (info) => {
    const metadata = new grpc.Metadata();
    for (var f in info) {
        metadata.add(`nebula-${f}`, `${info[f]}`);
    }
    return metadata;
};

/** 
 * get all available tables in nebula.
 */
const getTables = (q, handler) => {
    const listReq = new NebulaClient.ListTables();
    listReq.setLimit(100);
    client.tables(listReq, {}, (err, reply) => {
        if (err !== null) {
            return handler.onError(err);
        }

        if (reply == null) {
            return handler.onNull();
        }
        return handler.onSuccess(JSON.stringify(reply.getTableList()));
    });
};

const getTableState = (q, handler) => {
    const req = new NebulaClient.TableStateRequest();
    req.setTable(q.table);
    client.state(req, {}, (err, reply) => {
        if (err !== null) {
            return handler.onError(err);
        }

        if (reply == null) {
            return handler.onNull();
        }
        return handler.onSuccess(JSON.stringify({
            bc: reply.getBlockcount(),
            rc: reply.getRowcount(),
            ms: reply.getMemsize(),
            mt: reply.getMintime(),
            xt: reply.getMaxtime(),
            dl: reply.getDimensionList(),
            ml: reply.getMetricList()
        }));
    });
}

/**
 * Generic web query routing.
 * This supports invoking nebula service behind web server rather than from web client directly.
 * Please refer two different modes on how to architecture web component in Nebula.
 */
const webq = (q, handler) => {
    // the query object schema is defined in state.js
    const state = JSON.parse(q.query);
    const req = new NebulaClient.QueryRequest();
    req.setTable(state.table);
    req.setStart(seconds(state.start));
    req.setEnd(seconds(state.end));

    // the filter can be much more complex
    const filter = state.filter;
    if (filter) {
        // all rules under this group
        const rules = filter.r;
        if (rules && rules.length > 0) {
            const predicates = [];
            rules.forEach(r => {
                const pred = new NebulaClient.Predicate();
                if (r.v && r.v.length > 0) {
                    pred.setColumn(r.c);
                    pred.setOp(r.o);
                    pred.setValueList(r.v);
                    predicates.push(pred);
                }
            });

            if (predicates.length > 0) {
                if (filter.l === "AND") {
                    const f = new NebulaClient.PredicateAnd();
                    f.setExpressionList(predicates);
                    req.setFiltera(f);
                } else if (filter.l === "OR") {
                    const f = new NebulaClient.PredicateOr();
                    f.setExpressionList(predicates);
                    req.setFiltero(f);
                }
            }
        }
    }

    // set dimension
    const keys = state.keys;
    const display = state.display;
    if (display == NebulaClient.DisplayType.SAMPLES) {
        keys.unshift(timeCol);
    }
    req.setDimensionList(keys);


    // set query type and window
    req.setDisplay(display);
    req.setWindow(state.window);

    // TODO(cao) - this part logic does not exist in web mode (arch=1)
    const columns = state.customs;
    if (columns || columns.length > 0) {
        const cc = [];
        columns.forEach(c => {
            const col = new NebulaClient.CustomColumn();
            col.setColumn(c.name);
            col.setExpr(c.expr);
            col.setType(c.type);
            cc.push(col);
        });
        req.setCustomList(cc);
    }

    // set metric for non-samples query 
    // (use implicit type convert != instead of !==)
    if (display != NebulaClient.DisplayType.SAMPLES) {
        const m = new NebulaClient.Metric();
        const mcol = state.metrics;
        m.setColumn(mcol);
        m.setMethod(state.rollup);
        req.setMetricList([m]);

        // set order on metric only means we don't order on samples for now
        const o = new NebulaClient.Order();
        o.setColumn(mcol);
        o.setType(state.sort);
        req.setOrder(o);
    }

    // set limit
    req.setTop(state.limit);

    // send request to service to get result
    client.query(req, handler.metadata, (err, reply) => {
        if (err !== null) {
            return handler.onError(err);
        }

        if (reply == null) {
            return handler.onNull();
        }
        const stats = reply.getStats();
        return handler.onSuccess(
            JSON.stringify({
                error: stats.getError(),
                duration: stats.getQuerytimems(),
                data: Array.from(reply.getData())
            }));
    });
};

const api_handlers = {
    // system API supporting proxy query to nebula server
    "tables": getTables,
    "state": getTableState,
    "query": webq,

    // TODO(cao): move these customized API into a plugin module for extension
    "comments_by_userid": getCommentsByUserId,
    "comments_by_pinsignature": getCommentsByPinSignature,
    "comments_by_pattern": getCommentsByPattern,
    "pins_by_signature": getPinsBySignature,
    "signature_of_pin": getSignatureByPin,
    "keyed_pins_per_domain": getPinsPerDomainWithKey,
    "keyed_advertisers": getAdvertisersMatchingKey,
    "advertisers_spend": getAdvertisersSpend,
    "keyed_advertisers_with_spend": getAdvertisersWithSpendByKey,
    "search_promoted_pins": getPinsByKeyword
};

/**
 * Shut down nebula server and its nodes.
 * Used for debugging and profiling purpose.
 */
const shutdown = () => {
    const req = new NebulaClient.EchoRequest();
    req.setName("_nuclear_");

    // do the query
    client.nuclear(req, {}, (err, reply) => {
        console.log(`ERR=${err}, REP=${reply.getMessage()}`);
    });

    return JSON.stringify({
        state: 'OK'
    });
};

/**
 * On-demand loading data from parameters.
 * ?api=load&table=x&json=xxx&ttl=5000
 */
const load = (table, json, ttl, handler) => {
    const req = new NebulaClient.LoadRequest();
    if (!table) {
        handler.endWithMessage("Missing table");
        return;
    }
    req.setTable(table);

    if (!json) {
        handler.endWithMessage("Missing params json.");
        return;
    }
    req.setParamsjson(json);

    req.setTtl(ttl);

    // send the query
    client.load(req, {}, (err, reply) => {
        if (err !== null) {
            return handler.onError(err);
        }

        if (reply == null) {
            return handler.onNull();
        }
        return handler.onSuccess(JSON.stringify({
            er: reply.getError(),
            tb: reply.getTable(),
            ms: reply.getLoadtimems()
        }));
    });
};

//create a server object listening at 80:
http.createServer(async function (req, res) {
    const q = url.parse(req.url, true).query;
    if (q.api) {
        res.writeHead(200, {
            'Content-Type': 'application/json'
        });

        // routing generic query through web UI
        if (q.api === "list") {
            res.write(listApi(q));
        } else if (q.api === "user") {
            res.write(JSON.stringify(userInfo(q, req.headers)));
        } else if (q.api === "nuclear") {
            res.write(shutdown());
        } else if (q.api === "load") {
            const handler = new Handler();
            handler.response = res;
            return load(q.table, q.json, q.ttl || 3600, handler);
        } else if (q.api in api_handlers) {
            // basic requirement
            if (!q.start || !q.end) {
                res.write(error(`start and end time required for every api: ${q.api}`));
            } else {
                try {
                    // build a handler 
                    const handler = new Handler();
                    handler.response = res;
                    handler.metadata = toMetadata(userInfo(q, req.headers));
                    api_handlers[q.api](q, handler);
                    return;
                } catch (e) {
                    res.write(error(`api ${q.api} handler exception: ${e}`));
                }
            }
        } else {
            res.write(error(`API not found: ${q.api}`));
        }

        res.end();
        return;
    }

    // serving static resources
    NebulaClient.static_res(req, res);
}).listen(process.env.NODE_PORT || 80);